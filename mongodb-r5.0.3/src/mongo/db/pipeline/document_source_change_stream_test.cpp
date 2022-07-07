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

#include "mongo/platform/basic.h"

#include <boost/intrusive_ptr.hpp>
#include <memory>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_ensure_resume_token_present.h"
#include "mongo/db/pipeline/document_source_change_stream_oplog_match.h"
#include "mongo/db/pipeline/document_source_change_stream_transform.h"
#include "mongo/db/pipeline/document_source_change_stream_unwind_transactions.h"
#include "mongo/db/pipeline/document_source_check_invalidate.h"
#include "mongo/db/pipeline/document_source_check_resume_token.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_lookup_change_post_image.h"
#include "mongo/db/pipeline/document_source_lookup_change_pre_image.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/resharding/resharding_change_event_o2_field_gen.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

using boost::intrusive_ptr;
using repl::OplogEntry;
using repl::OpTypeEnum;
using std::list;
using std::string;
using std::vector;

using D = Document;
using V = Value;

using DSChangeStream = DocumentSourceChangeStream;

static const Timestamp kDefaultTs(100, 1);
static const repl::OpTime kDefaultOpTime(kDefaultTs, 1);
static const NamespaceString nss("unittests.change_stream");
static const BSONObj kDefaultSpec = fromjson("{$changeStream: {}}");

class ChangeStreamStageTestNoSetup : public AggregationContextFixture {
public:
    ChangeStreamStageTestNoSetup() : ChangeStreamStageTestNoSetup(nss) {}
    explicit ChangeStreamStageTestNoSetup(NamespaceString nsString)
        : AggregationContextFixture(nsString) {}
};

struct MockMongoInterface final : public StubMongoProcessInterface {

    // Used by operations which need to obtain the oplog's UUID.
    static const UUID& oplogUuid() {
        static const UUID* oplog_uuid = new UUID(UUID::gen());
        return *oplog_uuid;
    }

    // This mock iterator simulates a traversal of transaction history in the oplog by returning
    // mock oplog entries from a list.
    struct MockTransactionHistoryIterator : public TransactionHistoryIteratorBase {
        bool hasNext() const final {
            return (mockEntriesIt != mockEntries.end());
        }

        repl::OplogEntry next(OperationContext* opCtx) final {
            ASSERT(hasNext());
            return *(mockEntriesIt++);
        }

        repl::OpTime nextOpTime(OperationContext* opCtx) final {
            ASSERT(hasNext());
            return (mockEntriesIt++)->getOpTime();
        }

        std::vector<repl::OplogEntry> mockEntries;
        std::vector<repl::OplogEntry>::const_iterator mockEntriesIt;
    };

    MockMongoInterface(std::vector<FieldPath> fields,
                       std::vector<repl::OplogEntry> transactionEntries = {},
                       std::vector<Document> documentsForLookup = {})
        : _fields(std::move(fields)),
          _transactionEntries(std::move(transactionEntries)),
          _documentsForLookup{std::move(documentsForLookup)} {}

    // For tests of transactions that involve multiple oplog entries.
    std::unique_ptr<TransactionHistoryIteratorBase> createTransactionHistoryIterator(
        repl::OpTime time) const {
        auto iterator = std::make_unique<MockTransactionHistoryIterator>();

        // Simulate a lookup on the oplog timestamp by manually advancing the iterator until we
        // reach the desired timestamp.
        iterator->mockEntries = _transactionEntries;
        ASSERT(iterator->mockEntries.size() > 0);
        for (iterator->mockEntriesIt = iterator->mockEntries.begin();
             iterator->mockEntriesIt->getOpTime() != time;
             ++iterator->mockEntriesIt) {
            ASSERT(iterator->mockEntriesIt != iterator->mockEntries.end());
        }

        return iterator;
    }

    // Called by DocumentSourceLookupPreImage to obtain the UUID of the oplog. Since that's the only
    // piece of collection info we need for now, just return a BSONObj with the mock oplog UUID.
    BSONObj getCollectionOptions(OperationContext* opCtx, const NamespaceString& nss) {
        return BSON("uuid" << oplogUuid());
    }

    boost::optional<Document> lookupSingleDocument(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        UUID collectionUUID,
        const Document& documentKey,
        boost::optional<BSONObj> readConcern,
        bool allowSpeculativeMajorityRead) final {
        Matcher matcher(documentKey.toBson(), expCtx);
        auto it = std::find_if(_documentsForLookup.begin(),
                               _documentsForLookup.end(),
                               [&](const Document& lookedUpDoc) {
                                   return matcher.matches(lookedUpDoc.toBson(), nullptr);
                               });
        return (it != _documentsForLookup.end() ? *it : boost::optional<Document>{});
    }

    // For "insert" tests.
    std::pair<std::vector<FieldPath>, bool> collectDocumentKeyFieldsForHostedCollection(
        OperationContext*, const NamespaceString&, UUID) const final {
        return {_fields, false};
    }

    std::vector<FieldPath> _fields;

    // Stores oplog entries associated with a commit operation, including the oplog entries that a
    // real DocumentSourceChangeStream would not see, because they are marked with a "prepare" or
    // "partialTxn" flag. When the DocumentSourceChangeStream sees the commit for the transaction,
    // either an explicit "commitCommand" or an implicit commit represented by an "applyOps" that is
    // not marked with the "prepare" or "partialTxn" flag, it uses a TransactionHistoryIterator to
    // go back and look up these entries.
    //
    // These entries are stored in the order they would be returned by the
    // TransactionHistoryIterator, which is the _reverse_ of the order they appear in the oplog.
    std::vector<repl::OplogEntry> _transactionEntries;

    // These documents are used to feed the 'lookupSingleDocument' method.
    std::vector<Document> _documentsForLookup;
};

class ChangeStreamStageTest : public ChangeStreamStageTestNoSetup {
public:
    ChangeStreamStageTest() : ChangeStreamStageTest(nss) {
        // Initialize the UUID on the ExpressionContext, to allow tests with a resumeToken.
        getExpCtx()->uuid = testUuid();
    };

    explicit ChangeStreamStageTest(NamespaceString nsString)
        : ChangeStreamStageTestNoSetup(nsString) {
        repl::ReplicationCoordinator::set(getExpCtx()->opCtx->getServiceContext(),
                                          std::make_unique<repl::ReplicationCoordinatorMock>(
                                              getExpCtx()->opCtx->getServiceContext()));
    }

    void checkTransformation(const OplogEntry& entry,
                             const boost::optional<Document> expectedDoc,
                             std::vector<FieldPath> docKeyFields = {},
                             const BSONObj& spec = kDefaultSpec,
                             const boost::optional<Document> expectedInvalidate = {},
                             const std::vector<repl::OplogEntry> transactionEntries = {},
                             std::vector<Document> documentsForLookup = {}) {
        vector<intrusive_ptr<DocumentSource>> stages = makeStages(entry.getEntry().toBSON(), spec);
        auto lastStage = stages.back();

        getExpCtx()->mongoProcessInterface = std::make_unique<MockMongoInterface>(
            docKeyFields, transactionEntries, std::move(documentsForLookup));

        auto next = lastStage->getNext();
        // Match stage should pass the doc down if expectedDoc is given.
        ASSERT_EQ(next.isAdvanced(), static_cast<bool>(expectedDoc));
        if (expectedDoc) {
            ASSERT_DOCUMENT_EQ(next.releaseDocument(), *expectedDoc);
        }

        if (expectedInvalidate) {
            next = lastStage->getNext();
            ASSERT_TRUE(next.isAdvanced());
            ASSERT_DOCUMENT_EQ(next.releaseDocument(), *expectedInvalidate);

            // Then throw an exception on the next call of getNext().
            if (!feature_flags::gFeatureFlagChangeStreamsOptimization.isEnabledAndIgnoreFCV()) {
                ASSERT_THROWS(lastStage->getNext(), ExceptionFor<ErrorCodes::CloseChangeStream>);
            } else {
                ASSERT_THROWS(lastStage->getNext(),
                              ExceptionFor<ErrorCodes::ChangeStreamInvalidated>);
            }
        }
    }

    /**
     * Returns a list of stages expanded from a $changStream specification, starting with a
     * DocumentSourceMock which contains a single document representing 'entry'.
     */
    vector<intrusive_ptr<DocumentSource>> makeStages(const BSONObj& entry, const BSONObj& spec) {
        list<intrusive_ptr<DocumentSource>> result =
            DSChangeStream::createFromBson(spec.firstElement(), getExpCtx());
        vector<intrusive_ptr<DocumentSource>> stages(std::begin(result), std::end(result));
        getExpCtx()->mongoProcessInterface =
            std::make_unique<MockMongoInterface>(std::vector<FieldPath>{});

        // This match stage is a DocumentSourceOplogMatch, which we explicitly disallow from
        // executing as a safety mechanism, since it needs to use the collection-default collation,
        // even if the rest of the pipeline is using some other collation. To avoid ever executing
        // that stage here, we'll up-convert it from the non-executable DocumentSourceOplogMatch to
        // a fully-executable DocumentSourceMatch. This is safe because all of the unit tests will
        // use the 'simple' collation.
        auto match = dynamic_cast<DocumentSourceMatch*>(stages[0].get());
        ASSERT(match);
        auto executableMatch = DocumentSourceMatch::create(match->getQuery(), getExpCtx());
        // Replace the original match with the executable one.
        stages[0] = executableMatch;

        // Check the oplog entry is transformed correctly.
        auto transform = stages[2].get();
        ASSERT(transform);
        ASSERT_EQ(string(transform->getSourceName()), DSChangeStream::kStageName);

        // Create mock stage and insert at the front of the stages.
        auto mock = DocumentSourceMock::createForTest(D(entry), getExpCtx());
        stages.insert(stages.begin(), mock);

        // Remove the DSEnsureResumeTokenPresent stage since it will swallow the result.
        auto newEnd = std::remove_if(stages.begin(), stages.end(), [](auto& stage) {
            return dynamic_cast<DocumentSourceEnsureResumeTokenPresent*>(stage.get());
        });
        stages.erase(newEnd, stages.end());

        // Wire up the stages by setting the source stage.
        auto prevIt = stages.begin();
        for (auto stageIt = stages.begin() + 1; stageIt != stages.end(); stageIt++) {
            auto stage = (*stageIt).get();
            stage->setSource((*prevIt).get());
            prevIt = stageIt;
        }

        return stages;
    }

    vector<intrusive_ptr<DocumentSource>> makeStages(const OplogEntry& entry) {
        return makeStages(entry.getEntry().toBSON(), kDefaultSpec);
    }

    OplogEntry createCommand(const BSONObj& oField,
                             const boost::optional<UUID> uuid = boost::none,
                             const boost::optional<bool> fromMigrate = boost::none,
                             boost::optional<repl::OpTime> opTime = boost::none) {
        return makeOplogEntry(OpTypeEnum::kCommand,  // op type
                              nss.getCommandNS(),    // namespace
                              oField,                // o
                              uuid,                  // uuid
                              fromMigrate,           // fromMigrate
                              boost::none,           // o2
                              opTime);               // opTime
    }

    Document makeResumeToken(Timestamp ts,
                             ImplicitValue uuid = Value(),
                             ImplicitValue docKey = Value(),
                             ResumeTokenData::FromInvalidate fromInvalidate =
                                 ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                             size_t txnOpIndex = 0) {
        ResumeTokenData tokenData;
        tokenData.clusterTime = ts;
        tokenData.documentKey = docKey;
        tokenData.fromInvalidate = fromInvalidate;
        tokenData.txnOpIndex = txnOpIndex;
        if (!uuid.missing())
            tokenData.uuid = uuid.getUuid();
        return ResumeToken(tokenData).toDocument();
    }

    /**
     * Helper for running an applyOps through the pipeline, and getting all of the results.
     */
    std::vector<Document> getApplyOpsResults(const Document& applyOpsDoc,
                                             const LogicalSessionFromClient& lsid) {
        BSONObj applyOpsObj = applyOpsDoc.toBson();

        // Create an oplog entry and then glue on an lsid and txnNumber
        auto baseOplogEntry = makeOplogEntry(OpTypeEnum::kCommand,
                                             nss.getCommandNS(),
                                             applyOpsObj,
                                             testUuid(),
                                             boost::none,  // fromMigrate
                                             BSONObj());
        BSONObjBuilder builder(baseOplogEntry.getEntry().toBSON());
        builder.append("lsid", lsid.toBSON());
        builder.append("txnNumber", 0LL);
        BSONObj oplogEntry = builder.done();

        // Create the stages and check that the documents produced matched those in the applyOps.
        vector<intrusive_ptr<DocumentSource>> stages = makeStages(oplogEntry, kDefaultSpec);
        auto transform = stages[3].get();
        invariant(dynamic_cast<DocumentSourceChangeStreamTransform*>(transform) != nullptr);

        std::vector<Document> res;
        auto next = transform->getNext();
        while (next.isAdvanced()) {
            res.push_back(next.releaseDocument());
            next = transform->getNext();
        }
        return res;
    }


    /**
     * This method is required to avoid a static initialization fiasco resulting from calling
     * UUID::gen() in file static scope.
     */
    static const UUID& testUuid() {
        static const UUID* uuid_gen = new UUID(UUID::gen());
        return *uuid_gen;
    }

    static LogicalSessionFromClient testLsid() {
        // Required to avoid static initialization fiasco.
        static const UUID* uuid = new UUID(UUID::gen());
        LogicalSessionFromClient lsid{};
        lsid.setId(*uuid);
        return lsid;
    }

    /**
     * Creates an OplogEntry with given parameters and preset defaults for this test suite.
     */
    static repl::OplogEntry makeOplogEntry(
        repl::OpTypeEnum opType,
        NamespaceString nss,
        BSONObj object,
        boost::optional<UUID> uuid = testUuid(),
        boost::optional<bool> fromMigrate = boost::none,
        boost::optional<BSONObj> object2 = boost::none,
        boost::optional<repl::OpTime> opTime = boost::none,
        OperationSessionInfo sessionInfo = {},
        boost::optional<repl::OpTime> prevOpTime = {},
        boost::optional<repl::OpTime> preImageOpTime = boost::none) {
        long long hash = 1LL;
        return {
            repl::DurableOplogEntry(opTime ? *opTime : kDefaultOpTime,  // optime
                                    hash,                               // hash
                                    opType,                             // opType
                                    nss,                                // namespace
                                    uuid,                               // uuid
                                    fromMigrate,                        // fromMigrate
                                    repl::OplogEntry::kOplogVersion,    // version
                                    object,                             // o
                                    object2,                            // o2
                                    sessionInfo,                        // sessionInfo
                                    boost::none,                        // upsert
                                    Date_t(),                           // wall clock time
                                    {},                                 // statement ids
                                    prevOpTime,  // optime of previous write within same transaction
                                    preImageOpTime,  // pre-image optime
                                    boost::none,     // post-image optime
                                    boost::none,     // ShardId of resharding recipient
                                    boost::none,     // _id
                                    boost::none)};   // needsRetryImage
    }

    /**
     * Helper function to do a $v:2 delta oplog test.
     */
    void runUpdateV2OplogTest(BSONObj diff, Document updateModificationEntry) {
        BSONObj o2 = BSON("_id" << 1);
        auto deltaOplog = makeOplogEntry(OpTypeEnum::kUpdate,                // op type
                                         nss,                                // namespace
                                         BSON("diff" << diff << "$v" << 2),  // o
                                         testUuid(),                         // uuid
                                         boost::none,                        // fromMigrate
                                         o2);                                // o2
        // Update fields
        Document expectedUpdateField{
            {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o2)},
            {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
            {DSChangeStream::kClusterTimeField, kDefaultTs},
            {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
            {DSChangeStream::kDocumentKeyField, D{{"_id", 1}}},
            {
                "updateDescription",
                updateModificationEntry,
            },
        };
        checkTransformation(deltaOplog, expectedUpdateField);
    }
};

bool getCSOptimizationFeatureFlagValue() {
    return feature_flags::gFeatureFlagChangeStreamsOptimization.isEnabledAndIgnoreFCV();
}

/**
 * Runs the tests with feature flag 'featureFlagChangeStreamsOptimization' true and false.
 */
class ChangeStreamStageWithDualFeatureFlagValueTest : public ChangeStreamStageTest {
public:
    ChangeStreamStageWithDualFeatureFlagValueTest() : ChangeStreamStageTest() {}


    void run() {
        {
            RAIIServerParameterControllerForTest controller("featureFlagChangeStreamsOptimization",
                                                            true);
            ASSERT(getCSOptimizationFeatureFlagValue());
            ChangeStreamStageTest::run();
        }
        {
            RAIIServerParameterControllerForTest controller("featureFlagChangeStreamsOptimization",
                                                            false);
            ASSERT_FALSE(getCSOptimizationFeatureFlagValue());
            ChangeStreamStageTest::run();
        }
    }
};

TEST_F(ChangeStreamStageTest, ShouldRejectNonObjectArg) {
    auto expCtx = getExpCtx();

    ASSERT_THROWS_CODE(DSChangeStream::createFromBson(
                           BSON(DSChangeStream::kStageName << "invalid").firstElement(), expCtx),
                       AssertionException,
                       50808);

    ASSERT_THROWS_CODE(DSChangeStream::createFromBson(
                           BSON(DSChangeStream::kStageName << 12345).firstElement(), expCtx),
                       AssertionException,
                       50808);
}

TEST_F(ChangeStreamStageTest, ShouldRejectUnrecognizedOption) {
    auto expCtx = getExpCtx();

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName << BSON("unexpected" << 4)).firstElement(), expCtx),
        AssertionException,
        40415);

    // In older versions this option was accepted.
    ASSERT_THROWS_CODE(DSChangeStream::createFromBson(
                           BSON(DSChangeStream::kStageName << BSON(
                                    "$_resumeAfterClusterTime" << BSON("ts" << Timestamp(0, 1))))
                               .firstElement(),
                           expCtx),
                       AssertionException,
                       40415);
}

TEST_F(ChangeStreamStageTest, ShouldRejectNonStringFullDocumentOption) {
    auto expCtx = getExpCtx();

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName << BSON("fullDocument" << true)).firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::TypeMismatch);
}

TEST_F(ChangeStreamStageTest, ShouldRejectUnrecognizedFullDocumentOption) {
    auto expCtx = getExpCtx();

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(BSON(DSChangeStream::kStageName << BSON("fullDocument"
                                                                               << "unrecognized"))
                                           .firstElement(),
                                       expCtx),
        AssertionException,
        ErrorCodes::BadValue);
}

TEST_F(ChangeStreamStageTest, ShouldRejectBothStartAtOperationTimeAndResumeAfterOptions) {
    auto expCtx = getExpCtx();

    // Need to put the collection in the collection catalog so the resume token is valid.
    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(expCtx->opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(expCtx->opCtx, testUuid(), std::move(collection));
    });

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName
                 << BSON("resumeAfter"
                         << makeResumeToken(kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1))
                         << "startAtOperationTime" << kDefaultTs))
                .firstElement(),
            expCtx),
        AssertionException,
        40674);
}

TEST_F(ChangeStreamStageTest, ShouldRejectBothStartAfterAndResumeAfterOptions) {
    auto expCtx = getExpCtx();
    auto opCtx = expCtx->opCtx;

    // Need to put the collection in the collection catalog so the resume token is validcollection
    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(opCtx, testUuid(), std::move(collection));
    });

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName
                 << BSON("resumeAfter"
                         << makeResumeToken(kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1))
                         << "startAfter"
                         << makeResumeToken(kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1))))
                .firstElement(),
            expCtx),
        AssertionException,
        50865);
}

TEST_F(ChangeStreamStageTest, ShouldRejectBothStartAtOperationTimeAndStartAfterOptions) {
    auto expCtx = getExpCtx();
    auto opCtx = expCtx->opCtx;

    // Need to put the collection in the collection catalog so the resume token is valid.
    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(opCtx, testUuid(), std::move(collection));
    });

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName
                 << BSON("startAfter"
                         << makeResumeToken(kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1))
                         << "startAtOperationTime" << kDefaultTs))
                .firstElement(),
            expCtx),
        AssertionException,
        40674);
}

TEST_F(ChangeStreamStageTest, ShouldRejectResumeAfterWithResumeTokenMissingUUID) {
    auto expCtx = getExpCtx();
    auto opCtx = expCtx->opCtx;

    // Need to put the collection in the collection catalog so the resume token is valid.
    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(opCtx, testUuid(), std::move(collection));
    });

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName << BSON("resumeAfter" << makeResumeToken(kDefaultTs)))
                .firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::InvalidResumeToken);
}

TEST_F(ChangeStreamStageTestNoSetup, FailsWithNoReplicationCoordinator) {
    const auto spec = fromjson("{$changeStream: {}}");

    ASSERT_THROWS_CODE(DocumentSourceChangeStream::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       40573);
}

TEST_F(ChangeStreamStageTest, CannotCreateStageForSystemCollection) {
    auto expressionContext = getExpCtx();
    expressionContext->ns = NamespaceString{"db", "system.namespace"};
    const auto spec = fromjson("{$changeStream: {allowToRunOnSystemNS: false}}");
    ASSERT_THROWS_CODE(DocumentSourceChangeStream::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(ChangeStreamStageTest, CanCreateStageForSystemCollectionWhenAllowToRunOnSystemNSIsTrue) {
    auto expressionContext = getExpCtx();
    expressionContext->ns = NamespaceString{"db", "system.namespace"};
    expressionContext->inMongos = false;
    const auto spec = fromjson("{$changeStream: {allowToRunOnSystemNS: true}}");
    DocumentSourceChangeStream::createFromBson(spec.firstElement(), getExpCtx());
}

TEST_F(ChangeStreamStageTest,
       CannotCreateStageForSystemCollectionWhenAllowToRunOnSystemNSIsTrueAndInMongos) {
    auto expressionContext = getExpCtx();
    expressionContext->ns = NamespaceString{"db", "system.namespace"};
    expressionContext->inMongos = true;
    const auto spec = fromjson("{$changeStream: {allowToRunOnSystemNS: true}}");
    ASSERT_THROWS_CODE(DocumentSourceChangeStream::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(ChangeStreamStageTest, CanCreateStageForNonSystemCollection) {
    const auto spec = fromjson("{$changeStream: {}}");
    DocumentSourceChangeStream::createFromBson(spec.firstElement(), getExpCtx());
}

TEST_F(ChangeStreamStageTest, ShowMigrationsFailsOnMongos) {
    auto expCtx = getExpCtx();
    expCtx->inMongos = true;
    auto spec = fromjson("{$changeStream: {showMigrationEvents: true}}");

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(spec.firstElement(), expCtx), AssertionException, 31123);
}

TEST_F(ChangeStreamStageTest, TransformInsertDocKeyXAndId) {
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,           // op type
                                 nss,                           // namespace
                                 BSON("_id" << 1 << "x" << 2),  // o
                                 testUuid(),                    // uuid
                                 boost::none,                   // fromMigrate
                                 boost::none);                  // o2

    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"x", 2}, {"_id", 1}}},  // Note _id <-> x reversal.
    };
    checkTransformation(insert, expectedInsert, {{"x"}, {"_id"}});
    bool fromMigrate = false;  // also check actual "fromMigrate: false" not filtered
    auto insert2 = makeOplogEntry(insert.getOpType(),    // op type
                                  insert.getNss(),       // namespace
                                  insert.getObject(),    // o
                                  insert.getUuid(),      // uuid
                                  fromMigrate,           // fromMigrate
                                  insert.getObject2());  // o2
    checkTransformation(insert2, expectedInsert, {{"x"}, {"_id"}});
}

TEST_F(ChangeStreamStageTest, TransformInsertDocKeyIdAndX) {
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,           // op type
                                 nss,                           // namespace
                                 BSON("x" << 2 << "_id" << 1),  // o
                                 testUuid(),                    // uuid
                                 boost::none,                   // fromMigrate
                                 boost::none);                  // o2

    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), BSON("_id" << 1 << "x" << 2))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"x", 2}, {"_id", 1}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},  // _id first
    };
    checkTransformation(insert, expectedInsert, {{"_id"}, {"x"}});
}

TEST_F(ChangeStreamStageTest, TransformInsertDocKeyJustId) {
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,           // op type
                                 nss,                           // namespace
                                 BSON("_id" << 1 << "x" << 2),  // o
                                 testUuid(),                    // uuid
                                 boost::none,                   // fromMigrate
                                 boost::none);                  // o2

    Document expectedInsert{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), BSON("_id" << 1))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}}},
    };
    checkTransformation(insert, expectedInsert, {{"_id"}});
}

TEST_F(ChangeStreamStageTest, TransformInsertFromMigrate) {
    bool fromMigrate = true;
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,           // op type
                                 nss,                           // namespace
                                 BSON("_id" << 1 << "x" << 1),  // o
                                 boost::none,                   // uuid
                                 fromMigrate,                   // fromMigrate
                                 boost::none);                  // o2

    checkTransformation(insert, boost::none);
}

TEST_F(ChangeStreamStageTest, TransformInsertFromMigrateShowMigrations) {
    bool fromMigrate = true;
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,           // op type
                                 nss,                           // namespace
                                 BSON("x" << 2 << "_id" << 1),  // o
                                 testUuid(),                    // uuid
                                 fromMigrate,                   // fromMigrate
                                 boost::none);                  // o2

    auto spec = fromjson("{$changeStream: {showMigrationEvents: true}}");
    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), BSON("_id" << 1 << "x" << 2))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"x", 2}, {"_id", 1}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},  // _id first
    };
    checkTransformation(insert, expectedInsert, {{"_id"}, {"x"}}, spec);
}

TEST_F(ChangeStreamStageTest, TransformUpdateFields) {
    BSONObj o = BSON("$set" << BSON("y" << 1));
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto updateField = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o2);                  // o2

    // Update fields
    Document expectedUpdateField{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
        {
            "updateDescription",
            D{{"updatedFields", D{{"y", 1}}}, {"removedFields", vector<V>()}},
        },
    };
    checkTransformation(updateField, expectedUpdateField);
}

TEST_F(ChangeStreamStageTest, TransformSimpleDeltaOplogUpdatedFields) {
    BSONObj diff = BSON("u" << BSON("a" << 1 << "b"
                                        << "updated"));

    runUpdateV2OplogTest(diff,
                         D{{"updatedFields", D{{"a", 1}, {"b", "updated"_sd}}},
                           {"removedFields", vector<V>{}},
                           {"truncatedArrays", vector<V>{}}});
}

TEST_F(ChangeStreamStageTest, TransformSimpleDeltaOplogInsertFields) {
    BSONObj diff = BSON("i" << BSON("a" << 1 << "b"
                                        << "updated"));

    runUpdateV2OplogTest(diff,
                         D{{"updatedFields", D{{"a", 1}, {"b", "updated"_sd}}},
                           {"removedFields", vector<V>{}},
                           {"truncatedArrays", vector<V>{}}});
}

TEST_F(ChangeStreamStageTest, TransformSimpleDeltaOplogRemovedFields) {
    BSONObj diff = BSON("d" << BSON("a" << false << "b" << false));

    runUpdateV2OplogTest(diff,
                         D{{"updatedFields", D{}},
                           {"removedFields", vector<V>{V("a"_sd), V("b"_sd)}},
                           {"truncatedArrays", vector<V>{}}});
}

TEST_F(ChangeStreamStageTest, TransformComplexDeltaOplog) {
    BSONObj diff = fromjson(
        "{"
        "   d: { a: false, b: false },"
        "   u: { c: 1, d: \"updated\" },"
        "   i: { e: 2, f: 3 }"
        "}");

    runUpdateV2OplogTest(diff,
                         D{{"updatedFields", D{{"c", 1}, {"d", "updated"_sd}, {"e", 2}, {"f", 3}}},
                           {"removedFields", vector<V>{V("a"_sd), V("b"_sd)}},
                           {"truncatedArrays", vector<V>{}}});
}

TEST_F(ChangeStreamStageTest, TransformDeltaOplogSubObjectDiff) {
    BSONObj diff = fromjson(
        "{"
        "   u: { c: 1, d: \"updated\" },"
        "   ssubObj: {"
        "           d: { a: false, b: false },"
        "           u: { c: 1, d: \"updated\" }"
        "   }"
        "}");

    runUpdateV2OplogTest(
        diff,
        D{{"updatedFields",
           D{{"c", 1}, {"d", "updated"_sd}, {"subObj.c", 1}, {"subObj.d", "updated"_sd}}},
          {"removedFields", vector<V>{V("subObj.a"_sd), V("subObj.b"_sd)}},
          {"truncatedArrays", vector<V>{}}});
}

TEST_F(ChangeStreamStageTest, TransformDeltaOplogSubArrayDiff) {
    BSONObj diff = fromjson(
        "{"
        "   sarrField: {a: true, l: 10,"
        "           u0: 1,"
        "           u1: {a: 1}},"
        "   sarrField2: {a: true, l: 20}"
        "   }"
        "}");

    runUpdateV2OplogTest(diff,
                         D{{"updatedFields", D{{"arrField.0", 1}, {"arrField.1", D{{"a", 1}}}}},
                           {"removedFields", vector<V>{}},
                           {"truncatedArrays",
                            vector<V>{V{D{{"field", "arrField"_sd}, {"newSize", 10}}},
                                      V{D{{"field", "arrField2"_sd}, {"newSize", 20}}}}}});
}

TEST_F(ChangeStreamStageTest, TransformDeltaOplogSubArrayDiffWithEmptyStringField) {
    BSONObj diff = fromjson(
        "{"
        "   s: {a: true, l: 10,"
        "           u0: 1,"
        "           u1: {a: 1}}"
        "}");

    runUpdateV2OplogTest(
        diff,
        D{{"updatedFields", D{{".0", 1}, {".1", D{{"a", 1}}}}},
          {"removedFields", vector<V>{}},
          {"truncatedArrays", vector<V>{V{D{{"field", ""_sd}, {"newSize", 10}}}}}});
}

TEST_F(ChangeStreamStageTest, TransformDeltaOplogNestedComplexSubDiffs) {
    BSONObj diff = fromjson(
        "{"
        "   u: { a: 1, b: 2},"
        "   sarrField: {a: true, l: 10,"
        "           u0: 1,"
        "           u1: {a: 1},"
        "           s2: { u: {a: 1}},"  // "arrField.2.a" should be updated.
        "           u4: 1,"             // Test updating non-contiguous fields.
        "           u6: 2},"
        "   ssubObj: {"
        "           d: {b: false},"  // "subObj.b" should be removed.
        "           u: {a: 1}}"      // "subObj.a" should be updated.
        "}");

    runUpdateV2OplogTest(
        diff,
        D{{"updatedFields",
           D{
               {"a", 1},
               {"b", 2},
               {"arrField.0", 1},
               {"arrField.1", D{{"a", 1}}},
               {"arrField.2.a", 1},
               {"arrField.4", 1},
               {"arrField.6", 2},
               {"subObj.a", 1},
           }},
          {"removedFields", vector<V>{V("subObj.b"_sd)}},
          {"truncatedArrays", vector<V>{V{D{{"field", "arrField"_sd}, {"newSize", 10}}}}}});
}

// Legacy documents might not have an _id field; then the document key is the full (post-update)
// document.
TEST_F(ChangeStreamStageTest, TransformUpdateFieldsLegacyNoId) {
    BSONObj o = BSON("$set" << BSON("y" << 1));
    BSONObj o2 = BSON("x" << 1 << "y" << 1);
    auto updateField = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o2);                  // o2

    // Update fields
    Document expectedUpdateField{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"x", 1}, {"y", 1}}},
        {
            "updateDescription",
            D{{"updatedFields", D{{"y", 1}}}, {"removedFields", vector<V>()}},
        },
    };
    checkTransformation(updateField, expectedUpdateField);
}

TEST_F(ChangeStreamStageTest, TransformRemoveFields) {
    BSONObj o = BSON("$unset" << BSON("y" << 1));
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto removeField = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o2);                  // o2

    // Remove fields
    Document expectedRemoveField{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, Document{{"_id", 1}, {"x", 2}}},
        {
            "updateDescription",
            D{{"updatedFields", D{}}, {"removedFields", {"y"_sd}}},
        }};
    checkTransformation(removeField, expectedRemoveField);
}  // namespace

TEST_F(ChangeStreamStageTest, TransformReplace) {
    BSONObj o = BSON("_id" << 1 << "x" << 2 << "y" << 1);
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto replace = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                  nss,                  // namespace
                                  o,                    // o
                                  testUuid(),           // uuid
                                  boost::none,          // fromMigrate
                                  o2);                  // o2

    // Replace
    Document expectedReplace{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kReplaceOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}, {"y", 1}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
    };
    checkTransformation(replace, expectedReplace);
}

TEST_F(ChangeStreamStageTest, TransformDelete) {
    BSONObj o = BSON("_id" << 1 << "x" << 2);
    auto deleteEntry = makeOplogEntry(OpTypeEnum::kDelete,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      boost::none);         // o2

    // Delete
    Document expectedDelete{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDeleteOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
    };
    checkTransformation(deleteEntry, expectedDelete);

    bool fromMigrate = false;  // also check actual "fromMigrate: false" not filtered
    auto deleteEntry2 = makeOplogEntry(deleteEntry.getOpType(),    // op type
                                       deleteEntry.getNss(),       // namespace
                                       deleteEntry.getObject(),    // o
                                       deleteEntry.getUuid(),      // uuid
                                       fromMigrate,                // fromMigrate
                                       deleteEntry.getObject2());  // o2

    checkTransformation(deleteEntry2, expectedDelete);
}

TEST_F(ChangeStreamStageTest, TransformDeleteFromMigrate) {
    bool fromMigrate = true;
    auto deleteEntry = makeOplogEntry(OpTypeEnum::kDelete,  // op type
                                      nss,                  // namespace
                                      BSON("_id" << 1),     // o
                                      boost::none,          // uuid
                                      fromMigrate,          // fromMigrate
                                      boost::none);         // o2

    checkTransformation(deleteEntry, boost::none);
}

TEST_F(ChangeStreamStageTest, TransformDeleteFromMigrateShowMigrations) {
    bool fromMigrate = true;
    BSONObj o = BSON("_id" << 1);
    auto deleteEntry = makeOplogEntry(OpTypeEnum::kDelete,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      fromMigrate,          // fromMigrate
                                      boost::none);         // o2

    auto spec = fromjson("{$changeStream: {showMigrationEvents: true}}");
    Document expectedDelete{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDeleteOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}}},
    };

    checkTransformation(deleteEntry, expectedDelete, {}, spec);
}

TEST_F(ChangeStreamStageTest, TransformDrop) {
    OplogEntry dropColl = createCommand(BSON("drop" << nss.coll()), testUuid());

    Document expectedDrop{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDropCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
    };
    Document expectedInvalidate{
        {DSChangeStream::kIdField,
         makeResumeToken(
             kDefaultTs, testUuid(), Value(), ResumeTokenData::FromInvalidate::kFromInvalidate)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
    };

    checkTransformation(dropColl, expectedDrop, {}, kDefaultSpec, expectedInvalidate);
}

TEST_F(ChangeStreamStageTest, TransformRename) {
    NamespaceString otherColl("test.bar");
    OplogEntry rename =
        createCommand(BSON("renameCollection" << nss.ns() << "to" << otherColl.ns()), testUuid());

    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField,
         D{{"db", otherColl.db()}, {"coll", otherColl.coll()}}},
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
    };
    Document expectedInvalidate{
        {DSChangeStream::kIdField,
         makeResumeToken(
             kDefaultTs, testUuid(), Value(), ResumeTokenData::FromInvalidate::kFromInvalidate)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
    };

    checkTransformation(rename, expectedRename, {}, kDefaultSpec, expectedInvalidate);
}

TEST_F(ChangeStreamStageTest, TransformInvalidateFromMigrate) {
    NamespaceString otherColl("test.bar");

    bool dropCollFromMigrate = true;
    OplogEntry dropColl =
        createCommand(BSON("drop" << nss.coll()), testUuid(), dropCollFromMigrate);
    bool dropDBFromMigrate = true;
    OplogEntry dropDB = createCommand(BSON("dropDatabase" << 1), boost::none, dropDBFromMigrate);
    bool renameFromMigrate = true;
    OplogEntry rename =
        createCommand(BSON("renameCollection" << nss.ns() << "to" << otherColl.ns()),
                      boost::none,
                      renameFromMigrate);

    for (auto& entry : {dropColl, dropDB, rename}) {
        checkTransformation(entry, boost::none);
    }
}

TEST_F(ChangeStreamStageTest, TransformRenameTarget) {
    NamespaceString otherColl("test.bar");
    OplogEntry rename =
        createCommand(BSON("renameCollection" << otherColl.ns() << "to" << nss.ns()), testUuid());

    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", otherColl.db()}, {"coll", otherColl.coll()}}},
    };
    Document expectedInvalidate{
        {DSChangeStream::kIdField,
         makeResumeToken(
             kDefaultTs, testUuid(), Value(), ResumeTokenData::FromInvalidate::kFromInvalidate)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
    };

    checkTransformation(rename, expectedRename, {}, kDefaultSpec, expectedInvalidate);
}

TEST_F(ChangeStreamStageTest, MatchFiltersDropDatabaseCommand) {
    OplogEntry dropDB = createCommand(BSON("dropDatabase" << 1), boost::none, false);
    checkTransformation(dropDB, boost::none);
}

TEST_F(ChangeStreamStageTest, TransformNewShardDetected) {
    auto o2Field = D{{"type", "migrateChunkToNewShard"_sd}};
    auto newShardDetected = makeOplogEntry(OpTypeEnum::kNoop,
                                           nss,
                                           BSONObj(),
                                           testUuid(),
                                           boost::none,  // fromMigrate
                                           o2Field.toBson());

    Document expectedNewShardDetected{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), BSON("_id" << o2Field))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kNewShardDetectedOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
    };
    checkTransformation(newShardDetected, expectedNewShardDetected);
}

TEST_F(ChangeStreamStageTest, TransformReshardBegin) {
    auto uuid = UUID::gen();
    auto reshardingUuid = UUID::gen();

    ReshardingChangeEventO2Field o2Field{reshardingUuid, ReshardingChangeEventEnum::kReshardBegin};
    auto reshardingBegin = makeOplogEntry(OpTypeEnum::kNoop,
                                          nss,
                                          BSONObj(),
                                          uuid,
                                          true,  // fromMigrate
                                          o2Field.toBSON());

    auto spec = fromjson("{$changeStream: {showMigrationEvents: true}}");

    Document expectedReshardingBegin{
        {DSChangeStream::kReshardingUuidField, reshardingUuid},
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, uuid, BSON("_id" << o2Field.toBSON()))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kReshardBeginOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
    };
    checkTransformation(reshardingBegin, expectedReshardingBegin, {}, spec);
}

TEST_F(ChangeStreamStageTest, TransformReshardDoneCatchUp) {
    auto existingUuid = UUID::gen();
    auto reshardingUuid = UUID::gen();
    auto temporaryNs = constructTemporaryReshardingNss(nss.db(), existingUuid);

    ReshardingChangeEventO2Field o2Field{reshardingUuid,
                                         ReshardingChangeEventEnum::kReshardDoneCatchUp};
    auto reshardDoneCatchUp = makeOplogEntry(OpTypeEnum::kNoop,
                                             temporaryNs,
                                             BSONObj(),
                                             reshardingUuid,
                                             true,  // fromMigrate
                                             o2Field.toBSON());

    auto spec =
        fromjson("{$changeStream: {showMigrationEvents: true, allowToRunOnSystemNS: true}}");
    auto expCtx = getExpCtx();
    expCtx->ns = temporaryNs;

    Document expectedReshardingDoneCatchUp{
        {DSChangeStream::kReshardingUuidField, reshardingUuid},
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, reshardingUuid, BSON("_id" << o2Field.toBSON()))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kReshardDoneCatchUpOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
    };

    checkTransformation(reshardDoneCatchUp, expectedReshardingDoneCatchUp, {}, spec);
}

TEST_F(ChangeStreamStageTest, TransformEmptyApplyOps) {
    Document applyOpsDoc{{"applyOps", Value{std::vector<Document>{}}}};

    LogicalSessionFromClient lsid = testLsid();
    vector<Document> results = getApplyOpsResults(applyOpsDoc, lsid);

    // Should not return anything.
    ASSERT_EQ(results.size(), 0u);
}

DEATH_TEST_F(ChangeStreamStageTest, ShouldCrashWithNoopInsideApplyOps, "Unexpected noop") {
    Document applyOpsDoc =
        Document{{"applyOps",
                  Value{std::vector<Document>{
                      Document{{"op", "n"_sd},
                               {"ns", nss.ns()},
                               {"ui", testUuid()},
                               {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}}}}}};
    LogicalSessionFromClient lsid = testLsid();
    getApplyOpsResults(applyOpsDoc, lsid);  // Should crash.
}

DEATH_TEST_F(ChangeStreamStageTest,
             ShouldCrashWithEntryWithoutOpFieldInsideApplyOps,
             "Unexpected format for entry") {
    Document applyOpsDoc =
        Document{{"applyOps",
                  Value{std::vector<Document>{
                      Document{{"ns", nss.ns()},
                               {"ui", testUuid()},
                               {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}}}}}};
    LogicalSessionFromClient lsid = testLsid();
    getApplyOpsResults(applyOpsDoc, lsid);  // Should crash.
}

DEATH_TEST_F(ChangeStreamStageTest,
             ShouldCrashWithEntryWithNonStringOpFieldInsideApplyOps,
             "Unexpected format for entry") {
    Document applyOpsDoc =
        Document{{"applyOps",
                  Value{std::vector<Document>{
                      Document{{"op", 2},
                               {"ns", nss.ns()},
                               {"ui", testUuid()},
                               {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}}}}}};
    LogicalSessionFromClient lsid = testLsid();
    getApplyOpsResults(applyOpsDoc, lsid);  // Should crash.
}

TEST_F(ChangeStreamStageTest, TransformNonTransactionApplyOps) {
    BSONObj applyOpsObj = Document{{"applyOps",
                                    Value{std::vector<Document>{Document{
                                        {"op", "i"_sd},
                                        {"ns", nss.ns()},
                                        {"ui", testUuid()},
                                        {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}}}}}}
                              .toBson();

    // Don't append lsid or txnNumber

    auto oplogEntry = makeOplogEntry(OpTypeEnum::kCommand,
                                     nss.getCommandNS(),
                                     applyOpsObj,
                                     testUuid(),
                                     boost::none,  // fromMigrate
                                     BSONObj());


    checkTransformation(oplogEntry, boost::none);
}

TEST_F(ChangeStreamStageTest, TransformApplyOpsWithEntriesOnDifferentNs) {
    // Doesn't use the checkTransformation() pattern that other tests use since we expect multiple
    // documents to be returned from one applyOps.

    auto otherUUID = UUID::gen();
    Document applyOpsDoc{
        {"applyOps",
         Value{std::vector<Document>{
             Document{{"op", "i"_sd},
                      {"ns", "someotherdb.collname"_sd},
                      {"ui", otherUUID},
                      {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}},
             Document{{"op", "u"_sd},
                      {"ns", "someotherdb.collname"_sd},
                      {"ui", otherUUID},
                      {"o", Value{Document{{"$set", Value{Document{{"x", "hallo 2"_sd}}}}}}},
                      {"o2", Value{Document{{"_id", 123}}}}},
         }}},
    };
    LogicalSessionFromClient lsid = testLsid();
    vector<Document> results = getApplyOpsResults(applyOpsDoc, lsid);

    // All documents should be skipped.
    ASSERT_EQ(results.size(), 0u);
}

TEST_F(ChangeStreamStageTest, PreparedTransactionApplyOpsEntriesAreIgnored) {
    Document applyOpsDoc =
        Document{{"applyOps",
                  Value{std::vector<Document>{
                      Document{{"op", "i"_sd},
                               {"ns", nss.ns()},
                               {"ui", testUuid()},
                               {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}}}}},
                 {"prepare", true}};
    LogicalSessionFromClient lsid = testLsid();
    vector<Document> results = getApplyOpsResults(applyOpsDoc, lsid);

    // applyOps entries that are part of a prepared transaction are ignored. These entries will be
    // fetched for changeStreams delivery as part of transaction commit.
    ASSERT_EQ(results.size(), 0u);
}

TEST_F(ChangeStreamStageTest, CommitCommandReturnsOperationsFromPreparedTransaction) {
    // Create an oplog entry representing a prepared transaction.
    Document preparedApplyOps{
        {"applyOps",
         Value{std::vector<Document>{
             D{{"op", "i"_sd}, {"ns", nss.ns()}, {"ui", testUuid()}, {"o", V{D{{"_id", 123}}}}},
         }}},
        {"prepare", true},
    };

    repl::OpTime applyOpsOpTime(Timestamp(99, 1), 1);
    auto preparedTransaction = makeOplogEntry(OpTypeEnum::kCommand,
                                              nss.getCommandNS(),
                                              preparedApplyOps.toBson(),
                                              testUuid(),
                                              boost::none,  // fromMigrate
                                              boost::none,  // o2 field
                                              applyOpsOpTime);

    // Create an oplog entry representing the commit for the prepared transaction. The commit has a
    // 'prevWriteOpTimeInTransaction' value that matches the 'preparedApplyOps' entry, which the
    // MockMongoInterface will pretend is in the oplog.
    OperationSessionInfo sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());
    auto oplogEntry =
        repl::DurableOplogEntry(kDefaultOpTime,                   // optime
                                1LL,                              // hash
                                OpTypeEnum::kCommand,             // opType
                                nss.getCommandNS(),               // namespace
                                boost::none,                      // uuid
                                boost::none,                      // fromMigrate
                                repl::OplogEntry::kOplogVersion,  // version
                                BSON("commitTransaction" << 1),   // o
                                boost::none,                      // o2
                                sessionInfo,                      // sessionInfo
                                boost::none,                      // upsert
                                Date_t(),                         // wall clock time
                                {},                               // statement ids
                                applyOpsOpTime,  // optime of previous write within same transaction
                                boost::none,     // pre-image optime
                                boost::none,     // post-image optime
                                boost::none,     // ShardId of resharding recipient
                                boost::none,     // _id
                                boost::none);    // needsRetryImage

    // When the DocumentSourceChangeStreamTransform sees the "commitTransaction" oplog entry, we
    // expect it to return the insert op within our 'preparedApplyOps' oplog entry.
    Document expectedResult{
        {DSChangeStream::kTxnNumberField, static_cast<int>(*sessionInfo.getTxnNumber())},
        {DSChangeStream::kLsidField, Document{{sessionInfo.getSessionId()->toBSON()}}},
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), BSONObj())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"_id", 123}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{}},
    };

    checkTransformation(oplogEntry, expectedResult, {}, kDefaultSpec, {}, {preparedTransaction});
}

TEST_F(ChangeStreamStageTest, TransactionWithMultipleOplogEntries) {
    OperationSessionInfo sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());

    // Create two applyOps entries that together represent a whole transaction.
    repl::OpTime applyOpsOpTime1(Timestamp(100, 1), 1);
    Document applyOps1{
        {"applyOps",
         V{std::vector<Document>{
             D{{"op", "i"_sd},
               {"ns", nss.ns()},
               {"ui", testUuid()},
               {"o", V{Document{{"_id", 123}}}}},
             D{{"op", "i"_sd},
               {"ns", nss.ns()},
               {"ui", testUuid()},
               {"o", V{Document{{"_id", 456}}}}},
         }}},
        {"partialTxn", true},
    };

    auto transactionEntry1 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps1.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime1,
                                            sessionInfo,
                                            repl::OpTime());

    repl::OpTime applyOpsOpTime2(Timestamp(100, 2), 1);
    Document applyOps2{
        {"applyOps",
         V{std::vector<Document>{
             D{{"op", "i"_sd}, {"ns", nss.ns()}, {"ui", testUuid()}, {"o", V{D{{"_id", 789}}}}},
         }}},
        /* The absence of the "partialTxn" and "prepare" fields indicates that this command commits
           the transaction. */
    };

    auto transactionEntry2 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps2.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime2,
                                            sessionInfo,
                                            applyOpsOpTime1);

    // We do not use the checkTransformation() pattern that other tests use since we expect multiple
    // documents to be returned from one applyOps.
    auto stages = makeStages(transactionEntry2);
    auto transform = stages[3].get();
    invariant(dynamic_cast<DocumentSourceChangeStreamTransform*>(transform) != nullptr);

    // Populate the MockTransactionHistoryEditor in reverse chronological order.
    getExpCtx()->mongoProcessInterface = std::make_unique<MockMongoInterface>(
        std::vector<FieldPath>{},
        std::vector<repl::OplogEntry>{transactionEntry2, transactionEntry1});

    // We should get three documents from the change stream, based on the documents in the two
    // applyOps entries.
    auto next = transform->getNext();
    ASSERT(next.isAdvanced());
    auto nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 123);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    auto resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(resumeToken,
                       makeResumeToken(applyOpsOpTime2.getTimestamp(),
                                       testUuid(),
                                       V{D{}},
                                       ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                                       0));

    next = transform->getNext();
    ASSERT(next.isAdvanced());
    nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 456);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(resumeToken,
                       makeResumeToken(applyOpsOpTime2.getTimestamp(),
                                       testUuid(),
                                       V{D{}},
                                       ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                                       1));

    next = transform->getNext();
    ASSERT(next.isAdvanced());
    nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 789);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(resumeToken,
                       makeResumeToken(applyOpsOpTime2.getTimestamp(),
                                       testUuid(),
                                       V{D{}},
                                       ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                                       2));
}

TEST_F(ChangeStreamStageTest, TransactionWithEmptyOplogEntries) {
    OperationSessionInfo sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());

    // Create a transaction that is chained across 5 applyOps oplog entries. The first, third, and
    // final oplog entries in the transaction chain contain empty applyOps arrays. The test verifies
    // that change streams (1) correctly detect the transaction chain despite the fact that the
    // final applyOps, which implicitly commits the transaction, is empty; and (2) behaves correctly
    // upon encountering empty applyOps at other stages of the transaction chain.
    repl::OpTime applyOpsOpTime1(Timestamp(100, 1), 1);
    Document applyOps1{
        {"applyOps", V{std::vector<Document>{}}},
        {"partialTxn", true},
    };

    auto transactionEntry1 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps1.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime1,
                                            sessionInfo,
                                            repl::OpTime());

    repl::OpTime applyOpsOpTime2(Timestamp(100, 2), 1);
    Document applyOps2{
        {"applyOps",
         V{std::vector<Document>{
             D{{"op", "i"_sd},
               {"ns", nss.ns()},
               {"ui", testUuid()},
               {"o", V{Document{{"_id", 123}}}}},
         }}},
        {"partialTxn", true},
    };

    auto transactionEntry2 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps2.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime2,
                                            sessionInfo,
                                            applyOpsOpTime1);

    repl::OpTime applyOpsOpTime3(Timestamp(100, 3), 1);
    Document applyOps3{
        {"applyOps", V{std::vector<Document>{}}},
        {"partialTxn", true},
    };

    auto transactionEntry3 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps3.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime3,
                                            sessionInfo,
                                            applyOpsOpTime2);

    repl::OpTime applyOpsOpTime4(Timestamp(100, 4), 1);
    Document applyOps4{
        {"applyOps",
         V{std::vector<Document>{D{{"op", "i"_sd},
                                   {"ns", nss.ns()},
                                   {"ui", testUuid()},
                                   {"o", V{Document{{"_id", 456}}}}}}}},
        {"partialTxn", true},
    };

    auto transactionEntry4 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps4.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime4,
                                            sessionInfo,
                                            applyOpsOpTime3);

    repl::OpTime applyOpsOpTime5(Timestamp(100, 5), 1);
    Document applyOps5{
        {"applyOps", V{std::vector<Document>{}}},
        /* The absence of the "partialTxn" and "prepare" fields indicates that this command commits
           the transaction. */
    };

    auto transactionEntry5 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps5.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime5,
                                            sessionInfo,
                                            applyOpsOpTime4);

    // We do not use the checkTransformation() pattern that other tests use since we expect multiple
    // documents to be returned from one applyOps.
    auto stages = makeStages(transactionEntry5);
    auto transform = stages[3].get();
    invariant(dynamic_cast<DocumentSourceChangeStreamTransform*>(transform) != nullptr);

    // Populate the MockTransactionHistoryEditor in reverse chronological order.
    getExpCtx()->mongoProcessInterface =
        std::make_unique<MockMongoInterface>(std::vector<FieldPath>{},
                                             std::vector<repl::OplogEntry>{transactionEntry5,
                                                                           transactionEntry4,
                                                                           transactionEntry3,
                                                                           transactionEntry2,
                                                                           transactionEntry1});

    // We should get three documents from the change stream, based on the documents in the two
    // applyOps entries.
    auto next = transform->getNext();
    ASSERT(next.isAdvanced());
    auto nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 123);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    auto resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(resumeToken,
                       makeResumeToken(applyOpsOpTime5.getTimestamp(),
                                       testUuid(),
                                       V{D{}},
                                       ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                                       0));

    next = transform->getNext();
    ASSERT(next.isAdvanced());
    nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 456);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(resumeToken,
                       makeResumeToken(applyOpsOpTime5.getTimestamp(),
                                       testUuid(),
                                       V{D{}},
                                       ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                                       1));
}

TEST_F(ChangeStreamStageTest, TransactionWithOnlyEmptyOplogEntries) {
    OperationSessionInfo sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());

    // Create a transaction that is chained across 2 applyOps oplog entries. This test verifies that
    // a change stream correctly reads an empty transaction and does not observe any events from it.
    repl::OpTime applyOpsOpTime1(Timestamp(100, 1), 1);
    Document applyOps1{
        {"applyOps", V{std::vector<Document>{}}},
        {"partialTxn", true},
    };

    auto transactionEntry1 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps1.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime1,
                                            sessionInfo,
                                            repl::OpTime());

    repl::OpTime applyOpsOpTime2(Timestamp(100, 2), 1);
    Document applyOps2{
        {"applyOps", V{std::vector<Document>{}}},
        /* The absence of the "partialTxn" and "prepare" fields indicates that this command commits
           the transaction. */
    };

    auto transactionEntry2 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps2.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime2,
                                            sessionInfo,
                                            applyOpsOpTime1);

    // We do not use the checkTransformation() pattern that other tests use since we expect multiple
    // documents to be returned from one applyOps.
    auto stages = makeStages(transactionEntry2);
    auto transform = stages[3].get();
    invariant(dynamic_cast<DocumentSourceChangeStreamTransform*>(transform) != nullptr);

    // Populate the MockTransactionHistoryEditor in reverse chronological order.
    getExpCtx()->mongoProcessInterface = std::make_unique<MockMongoInterface>(
        std::vector<FieldPath>{},
        std::vector<repl::OplogEntry>{transactionEntry2, transactionEntry1});

    // We should get three documents from the change stream, based on the documents in the two
    // applyOps entries.
    auto next = transform->getNext();
    ASSERT(!next.isAdvanced());
}

TEST_F(ChangeStreamStageTest, PreparedTransactionWithMultipleOplogEntries) {
    OperationSessionInfo sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());

    // Create two applyOps entries that together represent a whole transaction.
    repl::OpTime applyOpsOpTime1(Timestamp(99, 1), 1);
    Document applyOps1{
        {"applyOps",
         V{std::vector<Document>{
             D{{"op", "i"_sd}, {"ns", nss.ns()}, {"ui", testUuid()}, {"o", V{D{{"_id", 123}}}}},
             D{{"op", "i"_sd}, {"ns", nss.ns()}, {"ui", testUuid()}, {"o", V{D{{"_id", 456}}}}},
         }}},
        {"partialTxn", true},
    };

    auto transactionEntry1 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps1.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime1,
                                            sessionInfo,
                                            repl::OpTime());

    repl::OpTime applyOpsOpTime2(Timestamp(99, 2), 1);
    Document applyOps2{
        {"applyOps",
         V{std::vector<Document>{
             D{{"op", "i"_sd}, {"ns", nss.ns()}, {"ui", testUuid()}, {"o", V{D{{"_id", 789}}}}},
         }}},
        {"prepare", true},
    };

    auto transactionEntry2 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps2.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime2,
                                            sessionInfo,
                                            applyOpsOpTime1);

    // Create an oplog entry representing the commit for the prepared transaction.
    auto commitEntry = repl::DurableOplogEntry(
        kDefaultOpTime,                   // optime
        1LL,                              // hash
        OpTypeEnum::kCommand,             // opType
        nss.getCommandNS(),               // namespace
        boost::none,                      // uuid
        boost::none,                      // fromMigrate
        repl::OplogEntry::kOplogVersion,  // version
        BSON("commitTransaction" << 1),   // o
        boost::none,                      // o2
        sessionInfo,                      // sessionInfo
        boost::none,                      // upsert
        Date_t(),                         // wall clock time
        {},                               // statement ids
        applyOpsOpTime2,                  // optime of previous write within same transaction
        boost::none,                      // pre-image optime
        boost::none,                      // post-image optime
        boost::none,                      // ShardId of resharding recipient
        boost::none,                      // _id
        boost::none);                     // needsRetryImage

    // We do not use the checkTransformation() pattern that other tests use since we expect multiple
    // documents to be returned from one applyOps.
    auto stages = makeStages(commitEntry);
    auto transform = stages[3].get();
    invariant(dynamic_cast<DocumentSourceChangeStreamTransform*>(transform) != nullptr);

    // Populate the MockTransactionHistoryEditor in reverse chronological order.
    getExpCtx()->mongoProcessInterface = std::make_unique<MockMongoInterface>(
        std::vector<FieldPath>{},
        std::vector<repl::OplogEntry>{commitEntry, transactionEntry2, transactionEntry1});

    // We should get three documents from the change stream, based on the documents in the two
    // applyOps entries.
    auto next = transform->getNext();
    ASSERT(next.isAdvanced());
    auto nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 123);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    auto resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(
        resumeToken,
        makeResumeToken(kDefaultOpTime.getTimestamp(),  // Timestamp of the commitCommand.
                        testUuid(),
                        V{D{}},
                        ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                        0));

    next = transform->getNext();
    ASSERT(next.isAdvanced());
    nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 456);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(
        resumeToken,
        makeResumeToken(kDefaultOpTime.getTimestamp(),  // Timestamp of the commitCommand.
                        testUuid(),
                        V{D{}},
                        ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                        1));

    next = transform->getNext();
    ASSERT(next.isAdvanced());
    nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 789);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(
        resumeToken,
        makeResumeToken(kDefaultOpTime.getTimestamp(),  // Timestamp of the commitCommand.
                        testUuid(),
                        V{D{}},
                        ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                        2));

    next = transform->getNext();
    ASSERT(!next.isAdvanced());
}

TEST_F(ChangeStreamStageTest, PreparedTransactionEndingWithEmptyApplyOps) {
    OperationSessionInfo sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());

    // Create two applyOps entries that together represent a whole transaction.
    repl::OpTime applyOpsOpTime1(Timestamp(99, 1), 1);
    Document applyOps1{
        {"applyOps",
         V{std::vector<Document>{
             D{{"op", "i"_sd}, {"ns", nss.ns()}, {"ui", testUuid()}, {"o", V{D{{"_id", 123}}}}},
             D{{"op", "i"_sd}, {"ns", nss.ns()}, {"ui", testUuid()}, {"o", V{D{{"_id", 456}}}}},
         }}},
        {"partialTxn", true},
    };

    auto transactionEntry1 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps1.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime1,
                                            sessionInfo,
                                            repl::OpTime());

    repl::OpTime applyOpsOpTime2(Timestamp(99, 2), 1);
    Document applyOps2{
        {"applyOps", V{std::vector<Document>{}}},
        {"prepare", true},
    };

    // The second applyOps is empty.
    auto transactionEntry2 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps2.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime2,
                                            sessionInfo,
                                            applyOpsOpTime1);

    // Create an oplog entry representing the commit for the prepared transaction.
    auto commitEntry = repl::DurableOplogEntry(
        kDefaultOpTime,                   // optime
        1LL,                              // hash
        OpTypeEnum::kCommand,             // opType
        nss.getCommandNS(),               // namespace
        boost::none,                      // uuid
        boost::none,                      // fromMigrate
        repl::OplogEntry::kOplogVersion,  // version
        BSON("commitTransaction" << 1),   // o
        boost::none,                      // o2
        sessionInfo,                      // sessionInfo
        boost::none,                      // upsert
        Date_t(),                         // wall clock time
        {},                               // statement ids
        applyOpsOpTime2,                  // optime of previous write within same transaction
        boost::none,                      // pre-image optime
        boost::none,                      // post-image optime
        boost::none,                      // ShardId of resharding recipient
        boost::none,                      // _id
        boost::none);                     // needsRetryImage

    // We do not use the checkTransformation() pattern that other tests use since we expect multiple
    // documents to be returned from one applyOps.
    auto stages = makeStages(commitEntry);
    auto transform = stages[3].get();
    invariant(dynamic_cast<DocumentSourceChangeStreamTransform*>(transform) != nullptr);

    // Populate the MockTransactionHistoryEditor in reverse chronological order.
    getExpCtx()->mongoProcessInterface = std::make_unique<MockMongoInterface>(
        std::vector<FieldPath>{},
        std::vector<repl::OplogEntry>{commitEntry, transactionEntry2, transactionEntry1});

    // We should get two documents from the change stream, based on the documents in the non-empty
    // applyOps entry.
    auto next = transform->getNext();
    ASSERT(next.isAdvanced());
    auto nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 123);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    auto resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(
        resumeToken,
        makeResumeToken(kDefaultOpTime.getTimestamp(),  // Timestamp of the commitCommand.
                        testUuid(),
                        V{D{}},
                        ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                        0));

    next = transform->getNext();
    ASSERT(next.isAdvanced());
    nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 456);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(
        resumeToken,
        makeResumeToken(kDefaultOpTime.getTimestamp(),  // Timestamp of the commitCommand.
                        testUuid(),
                        V{D{}},
                        ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                        1));

    next = transform->getNext();
    ASSERT(!next.isAdvanced());
}

TEST_F(ChangeStreamStageTest, TransformApplyOps) {
    // Doesn't use the checkTransformation() pattern that other tests use since we expect multiple
    // documents to be returned from one applyOps.

    Document applyOpsDoc{
        {"applyOps",
         Value{std::vector<Document>{
             Document{{"op", "i"_sd},
                      {"ns", nss.ns()},
                      {"ui", testUuid()},
                      {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}},
             Document{{"op", "u"_sd},
                      {"ns", nss.ns()},
                      {"ui", testUuid()},
                      {"o", Value{Document{{"$set", Value{Document{{"x", "hallo 2"_sd}}}}}}},
                      {"o2", Value{Document{{"_id", 123}}}}},
             // Operation on another namespace which should be skipped.
             Document{{"op", "i"_sd},
                      {"ns", "someotherdb.collname"_sd},
                      {"ui", UUID::gen()},
                      {"o", Value{Document{{"_id", 0}, {"x", "Should not read this!"_sd}}}}},
         }}},
    };
    LogicalSessionFromClient lsid = testLsid();
    vector<Document> results = getApplyOpsResults(applyOpsDoc, lsid);

    // The third document should be skipped.
    ASSERT_EQ(results.size(), 2u);

    // Check that the first document is correct.
    auto nextDoc = results[0];
    ASSERT_EQ(nextDoc["txnNumber"].getLong(), 0LL);
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 123);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["x"].getString(), "hallo");
    ASSERT_EQ(nextDoc["lsid"].getDocument().toBson().woCompare(lsid.toBSON()), 0);

    // Check the second document.
    nextDoc = results[1];
    ASSERT_EQ(nextDoc["txnNumber"].getLong(), 0LL);
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kUpdateOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kDocumentKeyField]["_id"].getInt(), 123);
    ASSERT_EQ(nextDoc[DSChangeStream::kUpdateDescriptionField]["updatedFields"]["x"].getString(),
              "hallo 2");
    ASSERT_EQ(nextDoc["lsid"].getDocument().toBson().woCompare(lsid.toBSON()), 0);

    // The third document is skipped.
}

TEST_F(ChangeStreamStageTest, ClusterTimeMatchesOplogEntry) {
    const Timestamp ts(3, 45);
    const long long term = 4;
    const auto opTime = repl::OpTime(ts, term);

    // Test the 'clusterTime' field is copied from the oplog entry for an update.
    BSONObj o = BSON("$set" << BSON("y" << 1));
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto updateField = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o2,                   // o2
                                      opTime);              // opTime

    Document expectedUpdateField{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kClusterTimeField, ts},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
        {
            "updateDescription",
            D{{"updatedFields", D{{"y", 1}}}, {"removedFields", vector<V>()}},
        },
    };
    checkTransformation(updateField, expectedUpdateField);

    // Test the 'clusterTime' field is copied from the oplog entry for a collection drop.
    OplogEntry dropColl =
        createCommand(BSON("drop" << nss.coll()), testUuid(), boost::none, opTime);

    Document expectedDrop{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDropCollectionOpType},
        {DSChangeStream::kClusterTimeField, ts},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
    };
    checkTransformation(dropColl, expectedDrop);

    // Test the 'clusterTime' field is copied from the oplog entry for a collection rename.
    NamespaceString otherColl("test.bar");
    OplogEntry rename =
        createCommand(BSON("renameCollection" << nss.ns() << "to" << otherColl.ns()),
                      testUuid(),
                      boost::none,
                      opTime);

    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField,
         D{{"db", otherColl.db()}, {"coll", otherColl.coll()}}},
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, ts},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
    };
    checkTransformation(rename, expectedRename);
}

TEST_F(ChangeStreamStageTest, MatchFiltersCreateCollection) {
    auto collSpec =
        D{{"create", "foo"_sd},
          {"idIndex", D{{"v", 2}, {"key", D{{"_id", 1}}}, {"name", "_id_"_sd}, {"ns", nss.ns()}}}};
    OplogEntry createColl = createCommand(collSpec.toBson(), testUuid());
    checkTransformation(createColl, boost::none);
}

TEST_F(ChangeStreamStageTest, MatchFiltersNoOp) {
    auto noOp = makeOplogEntry(OpTypeEnum::kNoop,  // op type
                               {},                 // namespace
                               BSON(repl::ReplicationCoordinator::newPrimaryMsgField
                                    << repl::ReplicationCoordinator::newPrimaryMsg));  // o

    checkTransformation(noOp, boost::none);
}

TEST_F(ChangeStreamStageWithDualFeatureFlagValueTest,
       TransformationShouldBeAbleToReParseSerializedStage) {
    auto expCtx = getExpCtx();
    const auto featureFlag = getCSOptimizationFeatureFlagValue();
    const auto serializedStageName =
        featureFlag ? DocumentSourceChangeStreamTransform::kStageName : DSChangeStream::kStageName;

    DocumentSourceChangeStreamSpec spec;
    spec.setStartAtOperationTime(kDefaultTs);
    auto originalSpec = BSON("" << spec.toBSON());

    auto result = DSChangeStream::createFromBson(originalSpec.firstElement(), expCtx);

    vector<intrusive_ptr<DocumentSource>> allStages(std::begin(result), std::end(result));

    const size_t changeStreamStageSize = featureFlag ? 5 : 6;
    ASSERT_EQ(allStages.size(), changeStreamStageSize);

    auto stage = allStages[2];
    ASSERT(dynamic_cast<DocumentSourceChangeStreamTransform*>(stage.get()));

    //
    // Serialize the stage and confirm contents.
    //
    vector<Value> serialization;
    stage->serializeToArray(serialization);
    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::Object);
    auto serializedDoc = serialization[0].getDocument();
    ASSERT_BSONOBJ_EQ(serializedDoc[serializedStageName].getDocument().toBson(),
                      originalSpec[""].Obj());

    //
    // Create a new stage from the serialization. Serialize the new stage and confirm that it is
    // equivalent to the original serialization.
    //
    auto serializedBson = serializedDoc.toBson();
    auto roundTripped = Pipeline::create(
        DSChangeStream::createFromBson(serializedBson.firstElement(), expCtx), expCtx);
    auto newSerialization = roundTripped->serialize();

    // When optimiziation is enabled, we should serialize all the internal stages.
    if (featureFlag) {
        ASSERT_EQ(newSerialization.size(), 5UL);

        // DSCSTransform stage should be the third stage after DSCSOplogMatch and
        // DSCSUnwindTransactions stages.
        ASSERT_VALUE_EQ(newSerialization[2], serialization[0]);
    } else {
        ASSERT_EQ(newSerialization.size(), 1UL);
        ASSERT_VALUE_EQ(newSerialization[0], serialization[0]);
    }
}

TEST_F(ChangeStreamStageWithDualFeatureFlagValueTest,
       DSCSTransformStageEmptySpecSerializeResumeAfter) {
    auto expCtx = getExpCtx();
    const auto serializedStageName = getCSOptimizationFeatureFlagValue()
        ? DocumentSourceChangeStreamTransform::kStageName
        : DSChangeStream::kStageName;

    auto originalSpec = BSON(DSChangeStream::kStageName << BSONObj());

    // Verify that the 'initialPostBatchResumeToken' is populated while parsing.
    ASSERT(expCtx->initialPostBatchResumeToken.isEmpty());
    ON_BLOCK_EXIT([&expCtx] {
        // Reset for the next run.
        expCtx->initialPostBatchResumeToken = BSONObj();
    });

    auto stage =
        DocumentSourceChangeStreamTransform::createFromBson(originalSpec.firstElement(), expCtx);
    ASSERT(!expCtx->initialPostBatchResumeToken.isEmpty());

    // Verify that an additional 'startAtOperationTime' is populated while serializing.
    vector<Value> serialization;
    stage->serializeToArray(serialization);
    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::Object);
    ASSERT(!serialization[0]
                .getDocument()[serializedStageName]
                .getDocument()[DocumentSourceChangeStreamSpec::kResumeAfterFieldName]
                .missing());
}

TEST_F(ChangeStreamStageWithDualFeatureFlagValueTest, DSCSTransformStageWithResumeTokenSerialize) {
    auto expCtx = getExpCtx();
    const auto serializedStageName = getCSOptimizationFeatureFlagValue()
        ? DocumentSourceChangeStreamTransform::kStageName
        : DSChangeStream::kStageName;

    DocumentSourceChangeStreamSpec spec;
    spec.setResumeAfter(ResumeToken::parse(makeResumeToken(kDefaultTs, testUuid())));
    auto originalSpec = BSON("" << spec.toBSON());

    // Verify that the 'initialPostBatchResumeToken' is populated while parsing.
    ASSERT(expCtx->initialPostBatchResumeToken.isEmpty());
    ON_BLOCK_EXIT([&expCtx] {
        // Reset for the next run.
        expCtx->initialPostBatchResumeToken = BSONObj();
    });

    auto stage =
        DocumentSourceChangeStreamTransform::createFromBson(originalSpec.firstElement(), expCtx);
    ASSERT(!expCtx->initialPostBatchResumeToken.isEmpty());

    vector<Value> serialization;
    stage->serializeToArray(serialization);
    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::Object);
    ASSERT_BSONOBJ_EQ(serialization[0].getDocument()[serializedStageName].getDocument().toBson(),
                      originalSpec[""].Obj());
}

template <typename Stage, typename StageSpec>
void validateDocumentSourceStageSerialization(
    StageSpec spec, BSONObj specAsBSON, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto stage = Stage::createFromBson(specAsBSON.firstElement(), expCtx);

    vector<Value> serialization;
    stage->serializeToArray(serialization);
    if (getCSOptimizationFeatureFlagValue()) {
        ASSERT_EQ(serialization.size(), 1UL);
        ASSERT_EQ(serialization[0].getType(), BSONType::Object);
        ASSERT_BSONOBJ_EQ(serialization[0].getDocument().toBson(),
                          BSON(Stage::kStageName << spec.toBSON()));
    } else {
        ASSERT(serialization.empty());
    }
}

TEST_F(ChangeStreamStageWithDualFeatureFlagValueTest, DSCSOplogMatchStageSerialization) {
    auto expCtx = getExpCtx();

    DocumentSourceChangeStreamOplogMatchSpec spec;
    auto dummyFilter = BSON("a" << 1);
    spec.setFilter(dummyFilter);
    auto stageSpecAsBSON = BSON("" << spec.toBSON());

    validateDocumentSourceStageSerialization<DocumentSourceOplogMatch>(
        std::move(spec), stageSpecAsBSON, expCtx);
}

TEST_F(ChangeStreamStageWithDualFeatureFlagValueTest, DSCSUnwindTransactionStageSerialization) {
    auto expCtx = getExpCtx();

    std::string nsRegex = "*.ns";
    DocumentSourceChangeStreamUnwindTransactionSpec spec(nsRegex);
    auto stageSpecAsBSON = BSON("" << spec.toBSON());

    validateDocumentSourceStageSerialization<DocumentSourceChangeStreamUnwindTransaction>(
        std::move(spec), stageSpecAsBSON, expCtx);
}

TEST_F(ChangeStreamStageWithDualFeatureFlagValueTest, DSCSCheckInvalidateStageSerialization) {
    auto expCtx = getExpCtx();

    DocumentSourceChangeStreamCheckInvalidateSpec spec;
    spec.setStartAfterInvalidate(ResumeToken::parse(makeResumeToken(
        kDefaultTs, testUuid(), Value(), ResumeTokenData::FromInvalidate::kFromInvalidate)));
    auto stageSpecAsBSON = BSON("" << spec.toBSON());

    validateDocumentSourceStageSerialization<DocumentSourceCheckInvalidate>(
        std::move(spec), stageSpecAsBSON, expCtx);
}

TEST_F(ChangeStreamStageWithDualFeatureFlagValueTest, DSCSResumabilityStageSerialization) {
    auto expCtx = getExpCtx();

    DocumentSourceChangeStreamCheckResumabilitySpec spec;
    spec.setResumeToken(ResumeToken::parse(makeResumeToken(kDefaultTs, testUuid())));
    auto stageSpecAsBSON = BSON("" << spec.toBSON());

    validateDocumentSourceStageSerialization<DocumentSourceCheckResumability>(
        std::move(spec), stageSpecAsBSON, expCtx);
}

TEST_F(ChangeStreamStageWithDualFeatureFlagValueTest, DSCSLookupChangePreImageStageSerialization) {
    auto expCtx = getExpCtx();

    DocumentSourceChangeStreamLookUpPreImageSpec spec(FullDocumentBeforeChangeModeEnum::kRequired);
    auto stageSpecAsBSON = BSON("" << spec.toBSON());

    validateDocumentSourceStageSerialization<DocumentSourceLookupChangePreImage>(
        std::move(spec), stageSpecAsBSON, expCtx);
}

TEST_F(ChangeStreamStageWithDualFeatureFlagValueTest, DSCSLookupChangePostImageStageSerialization) {
    auto expCtx = getExpCtx();

    DocumentSourceChangeStreamLookUpPostImageSpec spec(FullDocumentModeEnum::kUpdateLookup);
    auto stageSpecAsBSON = BSON("" << spec.toBSON());

    validateDocumentSourceStageSerialization<DocumentSourceLookupChangePostImage>(
        std::move(spec), stageSpecAsBSON, expCtx);
}

TEST_F(ChangeStreamStageTest, CloseCursorOnInvalidateEntries) {
    OplogEntry dropColl = createCommand(BSON("drop" << nss.coll()), testUuid());
    auto stages = makeStages(dropColl);
    auto lastStage = stages.back();

    Document expectedDrop{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDropCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
    };
    Document expectedInvalidate{
        {DSChangeStream::kIdField,
         makeResumeToken(
             kDefaultTs, testUuid(), Value(), ResumeTokenData::FromInvalidate::kFromInvalidate)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
    };

    auto next = lastStage->getNext();
    // Transform into drop entry.
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedDrop);
    next = lastStage->getNext();
    // Transform into invalidate entry.
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedInvalidate);

    // Then throw an exception on the next call of getNext().
    if (!feature_flags::gFeatureFlagChangeStreamsOptimization.isEnabledAndIgnoreFCV()) {
        ASSERT_THROWS(lastStage->getNext(), ExceptionFor<ErrorCodes::CloseChangeStream>);
    } else {
        ASSERT_THROWS(lastStage->getNext(), ExceptionFor<ErrorCodes::ChangeStreamInvalidated>);
    }
}

TEST_F(ChangeStreamStageTest, CloseCursorEvenIfInvalidateEntriesGetFilteredOut) {
    OplogEntry dropColl = createCommand(BSON("drop" << nss.coll()), testUuid());
    auto stages = makeStages(dropColl);
    auto lastStage = stages.back();
    // Add a match stage after change stream to filter out the invalidate entries.
    auto match = DocumentSourceMatch::create(fromjson("{operationType: 'insert'}"), getExpCtx());
    match->setSource(lastStage.get());

    // Throw an exception on the call of getNext().
    if (!feature_flags::gFeatureFlagChangeStreamsOptimization.isEnabledAndIgnoreFCV()) {
        ASSERT_THROWS(match->getNext(), ExceptionFor<ErrorCodes::CloseChangeStream>);
    } else {
        ASSERT_THROWS(match->getNext(), ExceptionFor<ErrorCodes::ChangeStreamInvalidated>);
    }
}

TEST_F(ChangeStreamStageTest, DocumentKeyShouldIncludeShardKeyFromResumeToken) {
    const Timestamp ts(3, 45);
    const long long term = 4;
    const auto opTime = repl::OpTime(ts, term);
    const auto uuid = testUuid();

    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(getExpCtx()->opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(getExpCtx()->opCtx, uuid, std::move(collection));
    });


    BSONObj o2 = BSON("_id" << 1 << "shardKey" << 2);
    auto resumeToken = makeResumeToken(ts, uuid, o2);

    BSONObj insertDoc = BSON("_id" << 2 << "shardKey" << 3);
    auto insertEntry = makeOplogEntry(OpTypeEnum::kInsert,  // op type
                                      nss,                  // namespace
                                      insertDoc,            // o
                                      uuid,                 // uuid
                                      boost::none,          // fromMigrate
                                      boost::none,          // o2
                                      opTime);              // opTime

    Document expectedInsert{
        {DSChangeStream::kIdField, makeResumeToken(ts, uuid, insertDoc)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, ts},
        {DSChangeStream::kFullDocumentField, D{{"_id", 2}, {"shardKey", 3}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 2}, {"shardKey", 3}}},
    };
    // Although the chunk manager and sharding catalog are not aware of the shard key in this test,
    // the expectation is for the $changeStream stage to infer the shard key from the resume token.
    checkTransformation(
        insertEntry,
        expectedInsert,
        {{"_id"}},  // Mock the 'collectDocumentKeyFieldsForHostedCollection' response.
        BSON("$changeStream" << BSON("resumeAfter" << resumeToken)));

    // Verify the same behavior with resuming using 'startAfter'.
    checkTransformation(
        insertEntry,
        expectedInsert,
        {{"_id"}},  // Mock the 'collectDocumentKeyFieldsForHostedCollection' response.
        BSON("$changeStream" << BSON("startAfter" << resumeToken)));
}

TEST_F(ChangeStreamStageTest, DocumentKeyShouldNotIncludeShardKeyFieldsIfNotPresentInOplogEntry) {
    const Timestamp ts(3, 45);
    const long long term = 4;
    const auto opTime = repl::OpTime(ts, term);
    const auto uuid = testUuid();

    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(getExpCtx()->opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(getExpCtx()->opCtx, uuid, std::move(collection));
    });

    BSONObj o2 = BSON("_id" << 1 << "shardKey" << 2);
    auto resumeToken = makeResumeToken(ts, uuid, o2);

    // Note that the 'o' field in the oplog entry does not contain the shard key field.
    BSONObj insertDoc = BSON("_id" << 2);
    auto insertEntry = makeOplogEntry(OpTypeEnum::kInsert,  // op type
                                      nss,                  // namespace
                                      insertDoc,            // o
                                      uuid,                 // uuid
                                      boost::none,          // fromMigrate
                                      boost::none,          // o2
                                      opTime);              // opTime

    Document expectedInsert{
        {DSChangeStream::kIdField, makeResumeToken(ts, uuid, insertDoc)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, ts},
        {DSChangeStream::kFullDocumentField, D{{"_id", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 2}}},
    };
    checkTransformation(
        insertEntry,
        expectedInsert,
        {{"_id"}},  // Mock the 'collectDocumentKeyFieldsForHostedCollection' response.
        BSON("$changeStream" << BSON("resumeAfter" << resumeToken)));

    // Verify the same behavior with resuming using 'startAfter'.
    checkTransformation(
        insertEntry,
        expectedInsert,
        {{"_id"}},  // Mock the 'collectDocumentKeyFieldsForHostedCollection' response.
        BSON("$changeStream" << BSON("startAfter" << resumeToken)));
}

TEST_F(ChangeStreamStageTest, ResumeAfterFailsIfResumeTokenDoesNotContainUUID) {
    const Timestamp ts(3, 45);
    const auto uuid = testUuid();

    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(getExpCtx()->opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(getExpCtx()->opCtx, uuid, std::move(collection));
    });

    // Create a resume token from only the timestamp.
    auto resumeToken = makeResumeToken(ts);

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName << BSON("resumeAfter" << resumeToken)).firstElement(),
            getExpCtx()),
        AssertionException,
        ErrorCodes::InvalidResumeToken);
}

TEST_F(ChangeStreamStageTest, RenameFromSystemToUserCollectionShouldIncludeNotification) {
    // Renaming to a non-system collection will include a notification in the stream.
    NamespaceString systemColl(nss.db() + ".system.users");
    OplogEntry rename =
        createCommand(BSON("renameCollection" << systemColl.ns() << "to" << nss.ns()), testUuid());

    // Note that the collection rename does *not* have the queued invalidated field.
    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", systemColl.db()}, {"coll", systemColl.coll()}}},
    };
    checkTransformation(rename, expectedRename);
}

TEST_F(ChangeStreamStageTest, RenameFromUserToSystemCollectionShouldIncludeNotification) {
    // Renaming to a system collection will include a notification in the stream.
    NamespaceString systemColl(nss.db() + ".system.users");
    OplogEntry rename =
        createCommand(BSON("renameCollection" << nss.ns() << "to" << systemColl.ns()), testUuid());

    // Note that the collection rename does *not* have the queued invalidated field.
    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField,
         D{{"db", systemColl.db()}, {"coll", systemColl.coll()}}},
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
    };
    checkTransformation(rename, expectedRename);
}

TEST_F(ChangeStreamStageTest, ResumeAfterWithTokenFromInvalidateShouldFail) {
    auto expCtx = getExpCtx();

    // Need to put the collection in the collection catalog so the resume token is valid.
    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(expCtx->opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(getExpCtx()->opCtx, testUuid(), std::move(collection));
    });

    const auto resumeTokenInvalidate =
        makeResumeToken(kDefaultTs,
                        testUuid(),
                        BSON("x" << 2 << "_id" << 1),
                        ResumeTokenData::FromInvalidate::kFromInvalidate);

    ASSERT_THROWS_CODE(DSChangeStream::createFromBson(
                           BSON(DSChangeStream::kStageName
                                << BSON("resumeAfter" << resumeTokenInvalidate
                                                      << "startAtOperationTime" << kDefaultTs))
                               .firstElement(),
                           expCtx),
                       AssertionException,
                       ErrorCodes::InvalidResumeToken);
}

TEST_F(ChangeStreamStageTest, UsesResumeTokenAsSortKeyIfNeedsMergeIsFalse) {
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,           // op type
                                 nss,                           // namespace
                                 BSON("x" << 2 << "_id" << 1),  // o
                                 testUuid(),                    // uuid
                                 boost::none,                   // fromMigrate
                                 boost::none);                  // o2

    auto stages = makeStages(insert.getEntry().toBSON(), kDefaultSpec);

    getExpCtx()->mongoProcessInterface =
        std::make_unique<MockMongoInterface>(std::vector<FieldPath>{{"x"}, {"_id"}});

    getExpCtx()->needsMerge = false;

    auto next = stages.back()->getNext();

    auto expectedSortKey = makeResumeToken(kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1));

    ASSERT_TRUE(next.isAdvanced());
    ASSERT_VALUE_EQ(next.releaseDocument().metadata().getSortKey(), Value(expectedSortKey));
}

//
// Test class for change stream of a single database.
//
class ChangeStreamStageDBTest : public ChangeStreamStageTest {
public:
    ChangeStreamStageDBTest()
        : ChangeStreamStageTest(NamespaceString::makeCollectionlessAggregateNSS(nss.db())) {}
};

TEST_F(ChangeStreamStageDBTest, TransformInsert) {
    auto insert = makeOplogEntry(OpTypeEnum::kInsert, nss, BSON("_id" << 1 << "x" << 2));

    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"x", 2}, {"_id", 1}}},  // Note _id <-> x reversal.
    };
    checkTransformation(insert, expectedInsert, {{"x"}, {"_id"}});
}

TEST_F(ChangeStreamStageDBTest, InsertOnOtherCollections) {
    NamespaceString otherNss("unittests.other_collection.");
    auto insertOtherColl =
        makeOplogEntry(OpTypeEnum::kInsert, otherNss, BSON("_id" << 1 << "x" << 2));

    // Insert on another collection in the same database.
    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", otherNss.db()}, {"coll", otherNss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"x", 2}, {"_id", 1}}},  // Note _id <-> x reversal.
    };
    checkTransformation(insertOtherColl, expectedInsert, {{"x"}, {"_id"}});
}

TEST_F(ChangeStreamStageDBTest, MatchFiltersChangesOnOtherDatabases) {
    std::set<NamespaceString> unmatchedNamespaces = {
        // Namespace starts with the db name, but is longer.
        NamespaceString("unittests2.coll"),
        // Namespace contains the db name, but not at the front.
        NamespaceString("test.unittests"),
        // Namespace contains the db name + dot.
        NamespaceString("test.unittests.coll"),
        // Namespace contains the db name + dot but is followed by $.
        NamespaceString("unittests.$cmd"),
    };

    // Insert into another database.
    for (auto& ns : unmatchedNamespaces) {
        auto insert = makeOplogEntry(OpTypeEnum::kInsert, ns, BSON("_id" << 1));
        checkTransformation(insert, boost::none);
    }
}

TEST_F(ChangeStreamStageDBTest, MatchFiltersAllSystemDotCollections) {
    auto nss = NamespaceString("unittests.system.coll");
    auto insert = makeOplogEntry(OpTypeEnum::kInsert, nss, BSON("_id" << 1));
    checkTransformation(insert, boost::none);

    nss = NamespaceString("unittests.system.users");
    insert = makeOplogEntry(OpTypeEnum::kInsert, nss, BSON("_id" << 1));
    checkTransformation(insert, boost::none);

    nss = NamespaceString("unittests.system.roles");
    insert = makeOplogEntry(OpTypeEnum::kInsert, nss, BSON("_id" << 1));
    checkTransformation(insert, boost::none);

    nss = NamespaceString("unittests.system.keys");
    insert = makeOplogEntry(OpTypeEnum::kInsert, nss, BSON("_id" << 1));
    checkTransformation(insert, boost::none);
}

TEST_F(ChangeStreamStageDBTest, TransformsEntriesForLegalClientCollectionsWithSystem) {
    std::set<NamespaceString> allowedNamespaces = {
        NamespaceString("unittests.coll.system"),
        NamespaceString("unittests.coll.system.views"),
        NamespaceString("unittests.systemx"),
    };

    for (auto& ns : allowedNamespaces) {
        auto insert = makeOplogEntry(OpTypeEnum::kInsert, ns, BSON("_id" << 1));
        Document expectedInsert{
            {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), BSON("_id" << 1))},
            {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
            {DSChangeStream::kClusterTimeField, kDefaultTs},
            {DSChangeStream::kFullDocumentField, D{{"_id", 1}}},
            {DSChangeStream::kNamespaceField, D{{"db", ns.db()}, {"coll", ns.coll()}}},
            {DSChangeStream::kDocumentKeyField, D{{"_id", 1}}},
        };
        checkTransformation(insert, expectedInsert, {{"_id"}});
    }
}

TEST_F(ChangeStreamStageDBTest, TransformUpdateFields) {
    BSONObj o = BSON("$set" << BSON("y" << 1));
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto updateField = makeOplogEntry(OpTypeEnum::kUpdate, nss, o, testUuid(), boost::none, o2);

    Document expectedUpdateField{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
        {"updateDescription", D{{"updatedFields", D{{"y", 1}}}, {"removedFields", vector<V>()}}},
    };
    checkTransformation(updateField, expectedUpdateField);
}

TEST_F(ChangeStreamStageDBTest, TransformRemoveFields) {
    BSONObj o = BSON("$unset" << BSON("y" << 1));
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto removeField = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o2);                  // o2

    // Remove fields
    Document expectedRemoveField{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
        {
            "updateDescription",
            D{{"updatedFields", D{}}, {"removedFields", {"y"_sd}}},
        }};
    checkTransformation(removeField, expectedRemoveField);
}

TEST_F(ChangeStreamStageDBTest, TransformReplace) {
    BSONObj o = BSON("_id" << 1 << "x" << 2 << "y" << 1);
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto replace = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                  nss,                  // namespace
                                  o,                    // o
                                  testUuid(),           // uuid
                                  boost::none,          // fromMigrate
                                  o2);                  // o2

    // Replace
    Document expectedReplace{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kReplaceOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}, {"y", 1}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
    };
    checkTransformation(replace, expectedReplace);
}

TEST_F(ChangeStreamStageDBTest, TransformDelete) {
    BSONObj o = BSON("_id" << 1 << "x" << 2);
    auto deleteEntry = makeOplogEntry(OpTypeEnum::kDelete,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      boost::none);         // o2

    // Delete
    Document expectedDelete{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDeleteOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
    };
    checkTransformation(deleteEntry, expectedDelete);

    bool fromMigrate = false;  // also check actual "fromMigrate: false" not filtered
    auto deleteEntry2 = makeOplogEntry(deleteEntry.getOpType(),    // op type
                                       deleteEntry.getNss(),       // namespace
                                       deleteEntry.getObject(),    // o
                                       deleteEntry.getUuid(),      // uuid
                                       fromMigrate,                // fromMigrate
                                       deleteEntry.getObject2());  // o2

    checkTransformation(deleteEntry2, expectedDelete);
}

TEST_F(ChangeStreamStageDBTest, TransformDeleteFromMigrate) {
    bool fromMigrate = true;
    auto deleteEntry = makeOplogEntry(OpTypeEnum::kDelete,  // op type
                                      nss,                  // namespace
                                      BSON("_id" << 1),     // o
                                      boost::none,          // uuid
                                      fromMigrate,          // fromMigrate
                                      boost::none);         // o2

    checkTransformation(deleteEntry, boost::none);
}

TEST_F(ChangeStreamStageDBTest, TransformDeleteFromMigrateShowMigrations) {
    bool fromMigrate = true;
    BSONObj o = BSON("_id" << 1 << "x" << 2);
    auto deleteEntry = makeOplogEntry(OpTypeEnum::kDelete,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      fromMigrate,          // fromMigrate
                                      boost::none);         // o2

    // Delete
    auto spec = fromjson("{$changeStream: {showMigrationEvents: true}}");
    Document expectedDelete{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDeleteOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
    };

    checkTransformation(deleteEntry, expectedDelete, {}, spec);
}

TEST_F(ChangeStreamStageDBTest, TransformDrop) {
    OplogEntry dropColl = createCommand(BSON("drop" << nss.coll()), testUuid());
    Document expectedDrop{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDropCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
    };
    checkTransformation(dropColl, expectedDrop);
}

TEST_F(ChangeStreamStageDBTest, TransformRename) {
    NamespaceString otherColl("test.bar");
    OplogEntry rename =
        createCommand(BSON("renameCollection" << nss.ns() << "to" << otherColl.ns()), testUuid());

    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField,
         D{{"db", otherColl.db()}, {"coll", otherColl.coll()}}},
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
    };
    checkTransformation(rename, expectedRename);
}

TEST_F(ChangeStreamStageDBTest, TransformDropDatabase) {
    OplogEntry dropDB = createCommand(BSON("dropDatabase" << 1), boost::none, false);

    // Drop database entry doesn't have a UUID.
    Document expectedDropDatabase{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDropDatabaseOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}}},
    };
    Document expectedInvalidate{
        {DSChangeStream::kIdField,
         makeResumeToken(
             kDefaultTs, Value(), Value(), ResumeTokenData::FromInvalidate::kFromInvalidate)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
    };

    checkTransformation(dropDB, expectedDropDatabase, {}, kDefaultSpec, expectedInvalidate);
}

TEST_F(ChangeStreamStageTest, TransformPreImageForDelete) {
    // Set the pre-image opTime to 1 second prior to the default event optime.
    repl::OpTime preImageOpTime{Timestamp(kDefaultTs.getSecs() - 1, 1), 1};
    const auto preImageObj = BSON("_id" << 1 << "x" << 2);

    // The documentKey for the main change stream event.
    const auto documentKey = BSON("_id" << 1);

    // The mock oplog UUID used by MockMongoInterface.
    auto oplogUUID = MockMongoInterface::oplogUuid();

    // Create an oplog entry for the pre-image no-op event.
    auto preImageEntry = makeOplogEntry(OpTypeEnum::kNoop,
                                        NamespaceString::kRsOplogNamespace,
                                        preImageObj,    // o
                                        oplogUUID,      // uuid
                                        boost::none,    // fromMigrate
                                        boost::none,    // o2
                                        preImageOpTime  // opTime
    );

    // Create an oplog entry for the delete event that will look up the pre-image.
    auto deleteEntry = makeOplogEntry(OpTypeEnum::kDelete,
                                      nss,
                                      documentKey,     // o
                                      testUuid(),      // uuid
                                      boost::none,     // fromMigrate
                                      boost::none,     // o2
                                      kDefaultOpTime,  // opTime
                                      {},              // sessionInfo
                                      {},              // prevOpTime
                                      preImageOpTime   // preImageOpTime
    );

    // Add the preImage oplog entry into a vector of documents that will be looked up. Add a dummy
    // entry before it so that we know we are finding the pre-image based on the given timestamp.
    repl::OpTime dummyOpTime{preImageOpTime.getTimestamp(), repl::OpTime::kInitialTerm};
    std::vector<Document> documentsForLookup = {Document{dummyOpTime.toBSON()},
                                                Document{preImageEntry.getEntry().toBSON()}};

    // When run with {fullDocumentBeforeChange: "off"}, we do not see a pre-image even if available.
    auto spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                             << "off"));
    Document expectedDeleteNoPreImage{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), documentKey)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDeleteOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, documentKey},
    };
    checkTransformation(
        deleteEntry, expectedDeleteNoPreImage, {}, spec, boost::none, {}, documentsForLookup);

    // When run with {fullDocumentBeforeChange: "whenAvailable"}, we see the pre-image.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "whenAvailable"));
    Document expectedDeleteWithPreImage{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), documentKey)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDeleteOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentBeforeChangeField, preImageObj},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, documentKey},
    };
    checkTransformation(
        deleteEntry, expectedDeleteWithPreImage, {}, spec, boost::none, {}, documentsForLookup);

    // When run with {fullDocumentBeforeChange: "required"}, we see the pre-image.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "required"));
    checkTransformation(
        deleteEntry, expectedDeleteWithPreImage, {}, spec, boost::none, {}, documentsForLookup);

    // When run with {fullDocumentBeforeChange: "whenAvailable"} but no pre-image, we see the event
    // without the pre-image.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "whenAvailable"));
    checkTransformation(deleteEntry, expectedDeleteNoPreImage, {}, spec);

    // When run with {fullDocumentBeforeChange: "required"} and a 'preImageOpTime' is present in the
    // event's oplog entry but we cannot find the pre-image, we throw ChangeStreamHistoryLost.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "required"));
    ASSERT_THROWS_CODE(checkTransformation(deleteEntry, boost::none, {}, spec),
                       AssertionException,
                       ErrorCodes::ChangeStreamHistoryLost);
}

TEST_F(ChangeStreamStageTest, TransformPreImageForUpdate) {
    // Set the pre-image opTime to 1 second prior to the default event optime.
    repl::OpTime preImageOpTime{Timestamp(kDefaultTs.getSecs() - 1, 1), 1};

    // Define the pre-image object, the update operation spec, and the document key.
    const auto updateSpec = BSON("$unset" << BSON("x" << 1));
    const auto preImageObj = BSON("_id" << 1 << "x" << 2);
    const auto documentKey = BSON("_id" << 1);

    // The mock oplog UUID used by MockMongoInterface.
    auto oplogUUID = MockMongoInterface::oplogUuid();

    // Create an oplog entry for the pre-image no-op event.
    auto preImageEntry = makeOplogEntry(OpTypeEnum::kNoop,
                                        NamespaceString::kRsOplogNamespace,
                                        preImageObj,    // o
                                        oplogUUID,      // uuid
                                        boost::none,    // fromMigrate
                                        boost::none,    // o2
                                        preImageOpTime  // opTime
    );

    // Create an oplog entry for the update event that will look up the pre-image.
    auto updateEntry = makeOplogEntry(OpTypeEnum::kUpdate,
                                      nss,
                                      updateSpec,      // o
                                      testUuid(),      // uuid
                                      boost::none,     // fromMigrate
                                      documentKey,     // o2
                                      kDefaultOpTime,  // opTime
                                      {},              // sessionInfo
                                      {},              // prevOpTime
                                      preImageOpTime   // preImageOpTime
    );

    // Add the preImage oplog entry into a vector of documents that will be looked up. Add a dummy
    // entry before it so that we know we are finding the pre-image based on the given timestamp.
    repl::OpTime dummyOpTime{preImageOpTime.getTimestamp(), repl::OpTime::kInitialTerm};
    std::vector<Document> documentsForLookup = {Document{dummyOpTime.toBSON()},
                                                Document{preImageEntry.getEntry().toBSON()}};

    // When run with {fullDocumentBeforeChange: "off"}, we do not see a pre-image even if available.
    auto spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                             << "off"));
    Document expectedUpdateNoPreImage{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), documentKey)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, documentKey},
        {
            "updateDescription",
            D{{"updatedFields", D{}}, {"removedFields", vector<V>{V("x"_sd)}}},
        },
    };
    checkTransformation(
        updateEntry, expectedUpdateNoPreImage, {}, spec, boost::none, {}, documentsForLookup);

    // When run with {fullDocumentBeforeChange: "whenAvailable"}, we see the pre-image.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "whenAvailable"));
    Document expectedUpdateWithPreImage{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), documentKey)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentBeforeChangeField, preImageObj},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, documentKey},
        {
            "updateDescription",
            D{{"updatedFields", D{}}, {"removedFields", vector<V>{V("x"_sd)}}},
        },
    };
    checkTransformation(
        updateEntry, expectedUpdateWithPreImage, {}, spec, boost::none, {}, documentsForLookup);

    // When run with {fullDocumentBeforeChange: "required"}, we see the pre-image.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "required"));
    checkTransformation(
        updateEntry, expectedUpdateWithPreImage, {}, spec, boost::none, {}, documentsForLookup);

    // When run with {fullDocumentBeforeChange: "whenAvailable"} but no pre-image, we see the event
    // without the pre-image.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "whenAvailable"));
    checkTransformation(updateEntry, expectedUpdateNoPreImage, {}, spec);

    // When run with {fullDocumentBeforeChange: "required"} and a 'preImageOpTime' is present in the
    // event's oplog entry but we cannot find the pre-image, we throw ChangeStreamHistoryLost.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "required"));
    ASSERT_THROWS_CODE(checkTransformation(updateEntry, boost::none, {}, spec),
                       AssertionException,
                       ErrorCodes::ChangeStreamHistoryLost);
}

TEST_F(ChangeStreamStageTest, TransformPreImageForReplace) {
    // Set the pre-image opTime to 1 second prior to the default event optime.
    repl::OpTime preImageOpTime{Timestamp(kDefaultTs.getSecs() - 1, 1), 1};

    // Define the pre-image object, the replacement document, and the document key.
    const auto replacementDoc = BSON("_id" << 1 << "y" << 3);
    const auto preImageObj = BSON("_id" << 1 << "x" << 2);
    const auto documentKey = BSON("_id" << 1);

    // The mock oplog UUID used by MockMongoInterface.
    auto oplogUUID = MockMongoInterface::oplogUuid();

    // Create an oplog entry for the pre-image no-op event.
    auto preImageEntry = makeOplogEntry(OpTypeEnum::kNoop,
                                        NamespaceString::kRsOplogNamespace,
                                        preImageObj,    // o
                                        oplogUUID,      // uuid
                                        boost::none,    // fromMigrate
                                        boost::none,    // o2
                                        preImageOpTime  // opTime
    );

    // Create an oplog entry for the replacement event that will look up the pre-image.
    auto replaceEntry = makeOplogEntry(OpTypeEnum::kUpdate,
                                       nss,
                                       replacementDoc,  // o
                                       testUuid(),      // uuid
                                       boost::none,     // fromMigrate
                                       documentKey,     // o2
                                       kDefaultOpTime,  // opTime
                                       {},              // sessionInfo
                                       {},              // prevOpTime
                                       preImageOpTime   // preImageOpTime
    );

    // Add the preImage oplog entry into a vector of documents that will be looked up. Add a dummy
    // entry before it so that we know we are finding the pre-image based on the given timestamp.
    repl::OpTime dummyOpTime{preImageOpTime.getTimestamp(), repl::OpTime::kInitialTerm};
    std::vector<Document> documentsForLookup = {Document{dummyOpTime.toBSON()},
                                                Document{preImageEntry.getEntry().toBSON()}};

    // When run with {fullDocumentBeforeChange: "off"}, we do not see a pre-image even if available.
    auto spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                             << "off"));
    Document expectedReplaceNoPreImage{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), documentKey)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kReplaceOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, replacementDoc},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, documentKey},
    };
    checkTransformation(
        replaceEntry, expectedReplaceNoPreImage, {}, spec, boost::none, {}, documentsForLookup);

    // When run with {fullDocumentBeforeChange: "whenAvailable"}, we see the pre-image.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "whenAvailable"));
    Document expectedReplaceWithPreImage{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), documentKey)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kReplaceOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, replacementDoc},
        {DSChangeStream::kFullDocumentBeforeChangeField, preImageObj},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, documentKey},
    };
    checkTransformation(
        replaceEntry, expectedReplaceWithPreImage, {}, spec, boost::none, {}, documentsForLookup);

    // When run with {fullDocumentBeforeChange: "required"}, we see the pre-image.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "required"));
    checkTransformation(
        replaceEntry, expectedReplaceWithPreImage, {}, spec, boost::none, {}, documentsForLookup);

    // When run with {fullDocumentBeforeChange: "whenAvailable"} but no pre-image, we see the event
    // without the pre-image.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "whenAvailable"));
    checkTransformation(replaceEntry, expectedReplaceNoPreImage, {}, spec);

    // When run with {fullDocumentBeforeChange: "required"} and a 'preImageOpTime' is present in the
    // event's oplog entry but we cannot find the pre-image, we throw ChangeStreamHistoryLost.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "required"));
    ASSERT_THROWS_CODE(checkTransformation(replaceEntry, boost::none, {}, spec),
                       AssertionException,
                       ErrorCodes::ChangeStreamHistoryLost);
}

TEST_F(ChangeStreamStageDBTest, MatchFiltersOperationsOnSystemCollections) {
    NamespaceString systemColl(nss.db() + ".system.users");
    OplogEntry insert = makeOplogEntry(OpTypeEnum::kInsert, systemColl, BSON("_id" << 1));
    checkTransformation(insert, boost::none);

    OplogEntry dropColl = createCommand(BSON("drop" << systemColl.coll()), testUuid());
    checkTransformation(dropColl, boost::none);

    // Rename from a 'system' collection to another 'system' collection should not include a
    // notification.
    NamespaceString renamedSystemColl(nss.db() + ".system.views");
    OplogEntry rename = createCommand(
        BSON("renameCollection" << systemColl.ns() << "to" << renamedSystemColl.ns()), testUuid());
    checkTransformation(rename, boost::none);
}

TEST_F(ChangeStreamStageDBTest, RenameFromSystemToUserCollectionShouldIncludeNotification) {
    // Renaming to a non-system collection will include a notification in the stream.
    NamespaceString systemColl(nss.db() + ".system.users");
    NamespaceString renamedColl(nss.db() + ".non_system_coll");
    OplogEntry rename = createCommand(
        BSON("renameCollection" << systemColl.ns() << "to" << renamedColl.ns()), testUuid());

    // Note that the collection rename does *not* have the queued invalidated field.
    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField,
         D{{"db", renamedColl.db()}, {"coll", renamedColl.coll()}}},
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", systemColl.db()}, {"coll", systemColl.coll()}}},
    };
    checkTransformation(rename, expectedRename);
}

TEST_F(ChangeStreamStageDBTest, RenameFromUserToSystemCollectionShouldIncludeNotification) {
    // Renaming to a system collection will include a notification in the stream.
    NamespaceString systemColl(nss.db() + ".system.users");
    OplogEntry rename =
        createCommand(BSON("renameCollection" << nss.ns() << "to" << systemColl.ns()), testUuid());

    // Note that the collection rename does *not* have the queued invalidated field.
    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField,
         D{{"db", systemColl.db()}, {"coll", systemColl.coll()}}},
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
    };
    checkTransformation(rename, expectedRename);
}

TEST_F(ChangeStreamStageDBTest, MatchFiltersNoOp) {
    OplogEntry noOp = makeOplogEntry(OpTypeEnum::kNoop,
                                     NamespaceString(),
                                     BSON(repl::ReplicationCoordinator::newPrimaryMsgField
                                          << repl::ReplicationCoordinator::newPrimaryMsg));
    checkTransformation(noOp, boost::none);
}

TEST_F(ChangeStreamStageDBTest, DocumentKeyShouldIncludeShardKeyFromResumeToken) {
    const Timestamp ts(3, 45);
    const long long term = 4;
    const auto opTime = repl::OpTime(ts, term);
    const auto uuid = testUuid();

    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(getExpCtx()->opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(getExpCtx()->opCtx, uuid, std::move(collection));
    });

    BSONObj o2 = BSON("_id" << 1 << "shardKey" << 2);
    auto resumeToken = makeResumeToken(ts, uuid, o2);

    BSONObj insertDoc = BSON("_id" << 2 << "shardKey" << 3);
    auto insertEntry = makeOplogEntry(OpTypeEnum::kInsert,  // op type
                                      nss,                  // namespace
                                      insertDoc,            // o
                                      uuid,                 // uuid
                                      boost::none,          // fromMigrate
                                      boost::none,          // o2
                                      opTime);              // opTime

    Document expectedInsert{
        {DSChangeStream::kIdField, makeResumeToken(ts, uuid, insertDoc)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, ts},
        {DSChangeStream::kFullDocumentField, D{{"_id", 2}, {"shardKey", 3}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 2}, {"shardKey", 3}}},
    };
    checkTransformation(
        insertEntry,
        expectedInsert,
        {{"_id"}},  // Mock the 'collectDocumentKeyFieldsForHostedCollection' response.
        BSON("$changeStream" << BSON("resumeAfter" << resumeToken)));
}

TEST_F(ChangeStreamStageDBTest, DocumentKeyShouldNotIncludeShardKeyFieldsIfNotPresentInOplogEntry) {
    const Timestamp ts(3, 45);
    const long long term = 4;
    const auto opTime = repl::OpTime(ts, term);
    const auto uuid = testUuid();

    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(getExpCtx()->opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(getExpCtx()->opCtx, uuid, std::move(collection));
    });

    BSONObj o2 = BSON("_id" << 1 << "shardKey" << 2);
    auto resumeToken = makeResumeToken(ts, uuid, o2);

    // Note that the 'o' field in the oplog entry does not contain the shard key field.
    BSONObj insertDoc = BSON("_id" << 2);
    auto insertEntry = makeOplogEntry(OpTypeEnum::kInsert,  // op type
                                      nss,                  // namespace
                                      insertDoc,            // o
                                      uuid,                 // uuid
                                      boost::none,          // fromMigrate
                                      boost::none,          // o2
                                      opTime);              // opTime

    Document expectedInsert{
        {DSChangeStream::kIdField, makeResumeToken(ts, uuid, insertDoc)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, ts},
        {DSChangeStream::kFullDocumentField, D{{"_id", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 2}}},
    };
    checkTransformation(
        insertEntry,
        expectedInsert,
        {{"_id"}},  // Mock the 'collectDocumentKeyFieldsForHostedCollection' response.
        BSON("$changeStream" << BSON("resumeAfter" << resumeToken)));
}

TEST_F(ChangeStreamStageDBTest, DocumentKeyShouldNotIncludeShardKeyIfResumeTokenDoesntContainUUID) {
    const Timestamp ts(3, 45);
    const long long term = 4;
    const auto opTime = repl::OpTime(ts, term);
    const auto uuid = testUuid();

    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(getExpCtx()->opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(getExpCtx()->opCtx, uuid, std::move(collection));
    });

    // Create a resume token from only the timestamp.
    auto resumeToken = makeResumeToken(ts);

    // Insert oplog entry contains shardKey, however we are not able to extract the shard key from
    // the resume token.
    BSONObj insertDoc = BSON("_id" << 2 << "shardKey" << 3);
    auto insertEntry = makeOplogEntry(OpTypeEnum::kInsert,  // op type
                                      nss,                  // namespace
                                      insertDoc,            // o
                                      uuid,                 // uuid
                                      boost::none,          // fromMigrate
                                      boost::none,          // o2
                                      opTime);              // opTime

    Document expectedInsert{
        {DSChangeStream::kIdField, makeResumeToken(ts, uuid, BSON("_id" << 2))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, ts},
        {DSChangeStream::kFullDocumentField, D{{"_id", 2}, {"shardKey", 3}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 2}}},
    };
    checkTransformation(
        insertEntry,
        expectedInsert,
        {{"_id"}},  // Mock the 'collectDocumentKeyFieldsForHostedCollection' response.
        BSON("$changeStream" << BSON("resumeAfter" << resumeToken)));
}

TEST_F(ChangeStreamStageDBTest, ResumeAfterWithTokenFromInvalidateShouldFail) {
    auto expCtx = getExpCtx();

    // Need to put the collection in the collection catalog so the resume token is valid.
    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(expCtx->opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(getExpCtx()->opCtx, testUuid(), std::move(collection));
    });

    const auto resumeTokenInvalidate =
        makeResumeToken(kDefaultTs,
                        testUuid(),
                        BSON("x" << 2 << "_id" << 1),
                        ResumeTokenData::FromInvalidate::kFromInvalidate);

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName << BSON("resumeAfter" << resumeTokenInvalidate))
                .firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::InvalidResumeToken);
}

TEST_F(ChangeStreamStageDBTest, ResumeAfterWithTokenFromDropDatabase) {
    const auto uuid = testUuid();

    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(getExpCtx()->opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(getExpCtx()->opCtx, uuid, std::move(collection));
    });

    // Create a resume token from only the timestamp, similar to a 'dropDatabase' entry.
    auto resumeToken = makeResumeToken(
        kDefaultTs, Value(), Value(), ResumeTokenData::FromInvalidate::kNotFromInvalidate);

    BSONObj insertDoc = BSON("_id" << 2);
    auto insertEntry = makeOplogEntry(OpTypeEnum::kInsert, nss, insertDoc);

    Document expectedInsert{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), insertDoc)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"_id", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 2}}},
    };
    checkTransformation(
        insertEntry,
        expectedInsert,
        {{"_id"}},  // Mock the 'collectDocumentKeyFieldsForHostedCollection' response.
        BSON("$changeStream" << BSON("resumeAfter" << resumeToken)));
}


TEST_F(ChangeStreamStageDBTest, StartAfterSucceedsEvenIfResumeTokenDoesNotContainUUID) {
    const auto uuid = testUuid();

    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(getExpCtx()->opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(getExpCtx()->opCtx, uuid, std::move(collection));
    });

    // Create a resume token from only the timestamp, similar to a 'dropDatabase' entry.
    auto resumeToken = makeResumeToken(kDefaultTs);

    BSONObj insertDoc = BSON("_id" << 2);
    auto insertEntry = makeOplogEntry(OpTypeEnum::kInsert, nss, insertDoc);

    Document expectedInsert{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, uuid, insertDoc)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"_id", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 2}}},
    };
    checkTransformation(
        insertEntry,
        expectedInsert,
        {{"_id"}},  // Mock the 'collectDocumentKeyFieldsForHostedCollection' response.
        BSON("$changeStream" << BSON("startAfter" << resumeToken)));
}

}  // namespace
}  // namespace mongo
