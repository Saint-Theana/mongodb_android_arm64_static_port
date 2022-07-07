/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/repl/idempotency_document_structure.h"
#include "mongo/db/repl/idempotency_test_fixture.h"
#include "mongo/db/repl/idempotency_update_sequence.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/db/update/document_diff_test_helpers.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

class RandomizedIdempotencyTest : public IdempotencyTest {
protected:
    const int kDocId = 1;
    const BSONObj kDocIdQuery = BSON("_id" << kDocId);

    std::vector<OplogEntry> createUpdateSequence(const UpdateSequenceGenerator& generator,
                                                 size_t length);

    BSONObj canonicalizeDocumentForDataHash(const BSONObj& obj) override;

    BSONObj getDoc();

    std::string getStatesString(const std::vector<CollectionState>& state1,
                                const std::vector<CollectionState>& state2,
                                const std::vector<OplogEntry>& state1Ops,
                                const std::vector<OplogEntry>& state2Ops) override;

    Status resetState() override;

    void runIdempotencyTestCase();
    void runUpdateV2IdempotencyTestCase(double v2Probability);

    std::vector<OplogEntry> initOps;
    int64_t seed;

private:
    // Op-style updates cannot guarantee field order for certain cases.
    bool _ignoreFieldOrder = true;
};

BSONObj canonicalizeBSONObjForDataHash(const BSONObj& obj);

BSONArray canonicalizeArrayForDataHash(const BSONObj& arr) {
    BSONArrayBuilder arrBuilder;
    for (auto&& elem : arr) {
        if (elem.type() == mongo::Array) {
            arrBuilder.append(canonicalizeArrayForDataHash(elem.embeddedObject()));
        } else if (elem.type() == mongo::Object) {
            arrBuilder.append(canonicalizeBSONObjForDataHash(elem.embeddedObject()));
        } else {
            arrBuilder.append(elem);
        }
    }
    return BSONArray(arrBuilder.obj());
}

BSONObj canonicalizeBSONObjForDataHash(const BSONObj& obj) {
    BSONObjBuilder objBuilder;
    BSONObjIteratorSorted iter(obj);
    while (iter.more()) {
        auto elem = iter.next();
        if (elem.isABSONObj()) {
            if (elem.type() == mongo::Array) {
                objBuilder.append(elem.fieldName(),
                                  canonicalizeArrayForDataHash(elem.embeddedObject()));
            } else {
                // If it is a sub object, we'll have to sort it as well before we append it.
                auto sortedObj = canonicalizeBSONObjForDataHash(elem.Obj());
                objBuilder.append(elem.fieldName(), sortedObj);
            }
        } else {
            // If it is not a sub object, just append it and move on.
            objBuilder.append(elem);
        }
    }
    return objBuilder.obj();
}

BSONObj RandomizedIdempotencyTest::canonicalizeDocumentForDataHash(const BSONObj& obj) {
    if (!_ignoreFieldOrder) {
        return obj;
    }
    return canonicalizeBSONObjForDataHash(obj);
}
BSONObj RandomizedIdempotencyTest::getDoc() {
    AutoGetCollectionForReadCommand autoColl(_opCtx.get(), nss);
    BSONObj doc;
    Helpers::findById(_opCtx.get(), autoColl.getDb(), nss.ns(), kDocIdQuery, doc);
    return doc.getOwned();
}

std::vector<OplogEntry> RandomizedIdempotencyTest::createUpdateSequence(
    const UpdateSequenceGenerator& generator, const size_t length) {
    // for each document enumerated & inserted generate a sequence of updates to apply to it.
    std::vector<OplogEntry> updateSequence;
    updateSequence.reserve(length);
    for (size_t i = 0; i < length; i++) {
        updateSequence.push_back(update(kDocId, generator.generateUpdate()));
    }

    return updateSequence;
}

std::string RandomizedIdempotencyTest::getStatesString(const std::vector<CollectionState>& state1,
                                                       const std::vector<CollectionState>& state2,
                                                       const std::vector<OplogEntry>& state1Ops,
                                                       const std::vector<OplogEntry>& state2Ops) {
    LOGV2(21157,
          "{IdempotencyTest_getStatesString_state1_state2_ops}",
          "IdempotencyTest_getStatesString_state1_state2_ops"_attr =
              IdempotencyTest::getStatesString(state1, state2, state1Ops, state2Ops));
    StringBuilder sb;
    sb << "Ran update ops: ";
    sb << "[ ";
    bool firstIter = true;
    for (const auto& op : state2Ops) {
        if (!firstIter) {
            sb << ", ";
        } else {
            firstIter = false;
        }
        sb << op.toStringForLogging();
    }
    sb << " ]\n";

    ASSERT_OK(resetState());
    ASSERT_OK(runOpsInitialSync(state1Ops));
    sb << "Document at the end of state1: " << getDoc() << "\n";

    ASSERT_OK(resetState());

    sb << "Start document for state2: " << getDoc() << "\n";
    for (const auto& op : state2Ops) {
        ASSERT_OK(runOpInitialSync(op));
        sb << "Apply: " << op.getObject() << "\n  ==> " << getDoc() << "\n";
    }

    sb << "Found from the seed: " << this->seed;

    return sb.str();
}

Status RandomizedIdempotencyTest::resetState() {
    Status dropStatus = runOpInitialSync(dropCollection());
    if (!dropStatus.isOK()) {
        return dropStatus;
    }

    return runOpsInitialSync(initOps);
}

void RandomizedIdempotencyTest::runIdempotencyTestCase() {
    _ignoreFieldOrder = true;
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    std::set<StringData> fields{"a", "b"};
    size_t depth = 2;
    const size_t lengthOfNumericComponent = 1;

    // Eliminate modification of array elements, because they cause theoretically valid sequences
    // that cause idempotency issues.
    const double kScalarProbability = 0.375;
    const double kDocProbability = 0.375;
    const double kArrProbability = 0;

    this->seed = SecureRandom().nextInt64();
    PseudoRandom seedGenerator(this->seed);
    RandomizedScalarGenerator scalarGenerator{PseudoRandom(seedGenerator.nextInt64())};
    UpdateSequenceGenerator updateGenerator({fields,
                                             depth,
                                             lengthOfNumericComponent,
                                             kScalarProbability,
                                             kDocProbability,
                                             kArrProbability},
                                            PseudoRandom{seedGenerator.nextInt64()},
                                            &scalarGenerator);

    const bool kSkipDocs = kDocProbability == 0.0;
    const bool kSkipArrs = kArrProbability == 0.0;
    DocumentStructureEnumerator enumerator(
        {fields, depth, lengthOfNumericComponent, kSkipDocs, kSkipArrs}, &scalarGenerator);

    const size_t kUpdateSequenceLength = 5;
    // For the sake of keeping the speed of iteration sane and feasible.
    const size_t kNumUpdateSequencesPerDoc = 2;

    for (auto doc : enumerator) {
        BSONObj docWithId = (BSONObjBuilder(doc) << "_id" << kDocId).obj();
        for (size_t i = 0; i < kNumUpdateSequencesPerDoc; i++) {
            this->initOps = std::vector<OplogEntry>{createCollection(), insert(docWithId)};
            std::vector<OplogEntry> updateSequence =
                createUpdateSequence(updateGenerator, kUpdateSequenceLength);
            testOpsAreIdempotent(updateSequence, SequenceType::kAnyPrefixOrSuffix);
        }
    }
}

void RandomizedIdempotencyTest::runUpdateV2IdempotencyTestCase(double v2Probability) {
    _ignoreFieldOrder = (v2Probability < 1.0);
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    this->seed = SecureRandom().nextInt64();
    PseudoRandom seedGenerator(this->seed);
    RandomizedScalarGenerator scalarGenerator{PseudoRandom(seedGenerator.nextInt64())};

    // Eliminate modification of array elements when generating $v:1 oplog udpates, because they
    // cause theoretically valid sequences that cause idempotency issues.
    //
    // For example oplog entries '{$unset: {a.1: null}}' and '{$set: {a.1.1: null}}' can break
    // idempotency if the entries are applied on an input document '{a: []}'. These entries should
    // not have been generated in practice if the starting document is '{a: []}', but the current
    // 'UpdateSequenceGenerator' is not smart enough to figure that out.
    const size_t lengthOfNumericComponent = 0;

    std::set<StringData> fields{"f00", "f10", "f01", "f11", "f02", "f20"};
    UpdateSequenceGenerator updateV1Generator({fields, 2 /* depth */, lengthOfNumericComponent},
                                              PseudoRandom(seedGenerator.nextInt64()),
                                              &scalarGenerator);

    auto generateDocWithId = [&seedGenerator](int id) {
        MutableDocument doc;
        doc.addField("_id", Value(id));
        PseudoRandom rng(seedGenerator.nextInt64());
        return doc_diff::generateDoc(&rng, &doc, 0);
    };

    PseudoRandom rng(seedGenerator.nextInt64());
    for (auto simulation = 0; simulation < 10; ++simulation) {
        // Initialize the collection with a single document, which would later be updated.
        auto inputObj = generateDocWithId(kDocId);
        this->initOps = std::vector<OplogEntry>{createCollection(), insert(inputObj)};
        ASSERT_OK(resetState());
        ASSERT_BSONOBJ_BINARY_EQ(inputObj, getDoc());

        auto oldDoc = inputObj;
        const size_t kUpdateSequenceLength = 15;
        std::vector<OplogEntry> updateSequence;
        for (size_t i = 0; i < kUpdateSequenceLength; i++) {
            BSONObj oplogDiff;
            boost::optional<BSONObj> generatedDoc;
            if (rng.nextCanonicalDouble() <= v2Probability) {
                // With delta based updates, we cannot just generate any random diff since certains
                // diff when applied to an unrelated object (which would never have produced by
                // computing the input objects) would break idempotency. So we do a dry run of what
                // the collection state would look like and compute diffs based on that.
                generatedDoc = generateDocWithId(kDocId);
                auto diffOutput =
                    doc_diff::computeDiff(oldDoc,
                                          *generatedDoc,
                                          update_oplog_entry::kSizeOfDeltaOplogEntryMetadata,
                                          nullptr);
                ASSERT(diffOutput);
                oplogDiff = BSON("$v" << 2 << "diff" << diffOutput->diff);
            } else {
                oplogDiff = updateV1Generator.generateUpdate();
            }
            auto op = update(kDocId, oplogDiff);
            ASSERT_OK(runOpInitialSync(op));
            if (generatedDoc) {
                ASSERT_BSONOBJ_BINARY_EQ(*generatedDoc, getDoc());
            }
            oldDoc = getDoc();
            updateSequence.push_back(std::move(op));
        }
        testOpsAreIdempotent(updateSequence, SequenceType::kAnyPrefixOrSuffix);
    }
}

TEST_F(RandomizedIdempotencyTest, CheckUpdateSequencesAreIdempotent) {
    runIdempotencyTestCase();
}
TEST_F(RandomizedIdempotencyTest, CheckUpdateSequencesAreIdempotentV2) {
    runUpdateV2IdempotencyTestCase(1.0);
    runUpdateV2IdempotencyTestCase(0.4);
    runUpdateV2IdempotencyTestCase(0.5);
    runUpdateV2IdempotencyTestCase(0.6);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
