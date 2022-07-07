/**
 * Compute the result of evaluating 'expression', and compare it to 'result'. Replaces the contents
 * of 'coll' with a single empty document.
 */
function testExpression(coll, expression, result) {
    testExpressionWithCollation(coll, expression, result);
}

/**
 * Compute the result of evaluating 'expression', and compare it to 'result', using 'collationSpec'
 * as the collation spec. Replaces the contents of 'coll' with a single empty document.
 */
function testExpressionWithCollation(coll, expression, result, collationSpec) {
    assert.commandWorked(coll.remove({}));
    assert.commandWorked(coll.insert({}));

    const options = collationSpec !== undefined ? {collation: collationSpec} : undefined;

    const res = coll.aggregate([{$project: {output: expression}}], options).toArray();

    assert.eq(res.length, 1, tojson(res));
    assert.eq(res[0].output, result, tojson(res));
}

/**
 * Returns true if 'al' is the same as 'ar'. If the two are arrays, the arrays can be in any order.
 * Objects (either 'al' and 'ar' themselves, or embedded objects) must have all the same properties.
 * If 'al' and 'ar' are neither object nor arrays, they must compare equal using 'valueComparator',
 * or == if not provided.
 */
function anyEq(al, ar, verbose = false, valueComparator, fieldsToSkip = []) {
    const debug = msg => verbose ? print(msg) : null;  // Helper to log 'msg' iff 'verbose' is true.

    if (al instanceof Array) {
        if (!(ar instanceof Array)) {
            debug('anyEq: ar is not an array ' + tojson(ar));
            return false;
        }

        if (!arrayEq(al, ar, verbose, valueComparator, fieldsToSkip)) {
            debug(`anyEq: arrayEq(al, ar): false; al=${tojson(al)}, ar=${tojson(ar)}`);
            return false;
        }
    } else if (al instanceof Object) {
        // Be sure to explicitly check for Arrays, since Arrays are considered instances of Objects,
        // and we do not want to consider [] to be equal to {}.
        if (!(ar instanceof Object) || (ar instanceof Array)) {
            debug('anyEq: ar is not an object ' + tojson(ar));
            return false;
        }

        if (!documentEq(al, ar, verbose, valueComparator, fieldsToSkip)) {
            debug(`anyEq: documentEq(al, ar): false; al=${tojson(al)}, ar=${tojson(ar)}`);
            return false;
        }
    } else if ((valueComparator && !valueComparator(al, ar)) || (!valueComparator && al !== ar)) {
        // Neither an object nor an array, use the custom comparator if provided.
        debug(`anyEq: (al != ar): false; al=${tojson(al)}, ar=${tojson(ar)}`);
        return false;
    }

    debug(`anyEq: these are equal: ${tojson(al)} == ${tojson(ar)}`);
    return true;
}

/**
 * Compares two documents for equality using a custom comparator for the values which returns true
 * or false. Returns true or false. Only equal if they have the exact same set of properties, and
 * all the properties' values match according to 'valueComparator'.
 */
function customDocumentEq({left, right, verbose, valueComparator, fieldsToSkip = []}) {
    return documentEq(left, right, verbose, valueComparator, fieldsToSkip);
}

/**
 * Compare two documents for equality. Returns true or false. Only equal if they have the exact same
 * set of properties, and all the properties' values match except the values with names in the
 * fieldsToSkip array. The fields in fieldsToSkip will be skipped at all levels of the document.
 * The value comparison with the recursive anyEq function allows for comparing nested array values
 * ignoring the elements' order.
 * If the order of the nested arrays elements is significant for the equivalence, the assert.docEq
 * from assert.js should be used instead.
 */
function documentEq(dl, dr, verbose = false, valueComparator, fieldsToSkip = []) {
    const debug = msg => verbose ? print(msg) : null;  // Helper to log 'msg' iff 'verbose' is true.

    // Make sure these are both objects.
    if (!(dl instanceof Object)) {
        debug('documentEq:  dl is not an object ' + tojson(dl));
        return false;
    }
    if (!(dr instanceof Object)) {
        debug('documentEq:  dr is not an object ' + tojson(dr));
        return false;
    }

    // Start by checking for all of dl's properties in dr.
    for (let propertyName in dl) {
        // Skip inherited properties.
        if (!dl.hasOwnProperty(propertyName))
            continue;

        // The documents aren't equal if they don't both have the property.
        if (!dr.hasOwnProperty(propertyName)) {
            debug('documentEq: dr doesn\'t have property ' + propertyName);
            return false;
        }

        if (fieldsToSkip.includes(propertyName))
            continue;

        if (!anyEq(dl[propertyName], dr[propertyName], verbose, valueComparator, fieldsToSkip)) {
            return false;
        }
    }

    // Now make sure that dr doesn't have any extras that dl doesn't have.
    for (let propertyName in dr) {
        if (!dr.hasOwnProperty(propertyName))
            continue;

        // If dl doesn't have this they are not equal; if it does, we compared it above and know it
        // to be equal.
        if (!dl.hasOwnProperty(propertyName)) {
            debug('documentEq: dl is missing property ' + propertyName);
            return false;
        }
    }

    debug(`documentEq: these are equal: ${tojson(dl)} == ${tojson(dr)}`);
    return true;
}

/**
 * Returns true if both 'al' and 'ar' are arrays of the same length with the same elements according
 * to valueComparator.  Order of the elements within the arrays is not significant.
 *
 * Element comparison uses the anyEq function recursively, which allows for comparing of nested
 * arrays with insignificant order.
 *
 * Use this function if the arguments have nested arrays and the element order is *not* significant
 * when the equivalence is determined. Use assert.sameMembers() in assert.js instead if the
 * arguments have no nested arrays, or the order of the nested arrays is significant for the
 * equivalent assertion.
 */
function arrayEq(al, ar, verbose = false, valueComparator, fieldsToSkip = []) {
    const debug = msg => verbose ? print(msg) : null;  // Helper to log 'msg' iff 'verbose' is true.

    // Check that these are both arrays.
    if (!(al instanceof Array)) {
        debug('arrayEq: al is not an array: ' + tojson(al));
        return false;
    }

    if (!(ar instanceof Array)) {
        debug('arrayEq: ar is not an array: ' + tojson(ar));
        return false;
    }

    if (al.length != ar.length) {
        debug(`arrayEq:  array lengths do not match ${tojson(al)}, ${tojson(ar)}`);
        return false;
    }

    // Keep a set of which indexes we've already used to avoid considering [1,1] as equal to [1,2].
    const matchedElementsInRight = new Set();
    for (let leftElem of al) {
        let foundMatch = false;
        for (let i = 0; i < ar.length; ++i) {
            if (!matchedElementsInRight.has(i) &&
                anyEq(leftElem, ar[i], verbose, valueComparator, fieldsToSkip)) {
                matchedElementsInRight.add(i);  // Don't use the same value each time.
                foundMatch = true;
                break;
            }
        }
        if (!foundMatch) {
            return false;
        }
    }

    return true;
}

function arrayDiff(al, ar, verbose = false, valueComparator, fieldsToSkip = []) {
    // Check that these are both arrays.
    if (!(al instanceof Array)) {
        debug('arrayDiff: al is not an array: ' + tojson(al));
        return false;
    }

    if (!(ar instanceof Array)) {
        debug('arrayDiff: ar is not an array: ' + tojson(ar));
        return false;
    }

    // Keep a set of which indexes we've already used to avoid considering [1,1] as equal to [1,2].
    const matchedIndexesInRight = new Set();
    let unmatchedElementsInLeft = [];
    for (let leftElem of al) {
        let foundMatch = false;
        for (let i = 0; i < ar.length; ++i) {
            if (!matchedIndexesInRight.has(i) &&
                anyEq(leftElem, ar[i], verbose, valueComparator, fieldsToSkip)) {
                matchedIndexesInRight.add(i);  // Don't use the same value each time.
                foundMatch = true;
                break;
            }
        }
        if (!foundMatch) {
            unmatchedElementsInLeft.push(leftElem);
        }
    }
    let unmatchedElementsInRight = [];
    for (let i = 0; i < ar.length; ++i) {
        if (!matchedIndexesInRight.has(i)) {
            unmatchedElementsInRight.push(ar[i]);
        }
    }
    return {left: unmatchedElementsInLeft, right: unmatchedElementsInRight};
}

/**
 * Makes a shallow copy of 'a'.
 */
function arrayShallowCopy(a) {
    assert(a instanceof Array, 'arrayShallowCopy: argument is not an array');
    return a.slice();  // Makes a copy.
}

/**
 * Compare two sets of documents (expressed as arrays) to see if they match. The two sets must have
 * the same documents, although the order need not match and values for fields defined in
 * "fieldsToSkip" need not match.
 *
 * Are non-scalar values references?
 */
function resultsEq(rl, rr, verbose = false, fieldsToSkip = []) {
    const debug = msg => verbose ? print(msg) : null;  // Helper to log 'msg' iff 'verbose' is true.

    // Make clones of the arguments so that we don't damage them.
    rl = arrayShallowCopy(rl);
    rr = arrayShallowCopy(rr);

    if (rl.length != rr.length) {
        debug(`resultsEq:  array lengths do not match ${tojson(rl)}, ${tojson(rr)}`);
        return false;
    }

    for (let i = 0; i < rl.length; ++i) {
        let foundIt = false;

        // Find a match in the other array.
        for (let j = 0; j < rr.length; ++j) {
            if (!anyEq(rl[i], rr[j], verbose, null, fieldsToSkip))
                continue;

            // Because we made the copies above, we can edit these out of the arrays so we don't
            // check on them anymore.
            // For the inner loop, we're going to be skipping out, so we don't need to be too
            // careful.
            rr.splice(j, 1);
            foundIt = true;
            break;
        }

        if (!foundIt) {
            // If we got here, we didn't find this item.
            debug(`resultsEq: search target missing index ${i} (${tojson(rl[i])})`);
            return false;
        }
    }

    assert(!rr.length);
    return true;
}

/**
 * Returns true if both 'al' and 'ar' are arrays of the same length with the same elements.
 * Order of the elements is significant only in the top-level arrays.
 *
 * Element comparison uses the anyEq function recursively, which allows for comparing of nested
 * arrays ignoring the elements' order.
 *
 * Use this function if the arguments have nested arrays and the elements' order is significant at
 * the top-level and insignificant for the nested arrays.
 */
function orderedArrayEq(al, ar, verbose = false, fieldsToSkip = []) {
    if (al.length != ar.length) {
        if (verbose)
            print(`orderedArrayEq:  array lengths do not match ${tojson(al)}, ${tojson(ar)}`);
        return false;
    }

    for (let i = 0; i < al.length; ++i) {
        if (!anyEq(al[i], ar[i], verbose, null, fieldsToSkip))
            return false;
    }

    return true;
}

/**
 * Assert that the given aggregation fails with a specific code. Error message is optional. Note
 * that 'code' can be an array of possible codes.
 */
function assertErrorCode(coll, pipe, code, errmsg, options = {}) {
    if (!Array.isArray(pipe)) {
        pipe = [pipe];
    }

    let cmd = {pipeline: pipe, cursor: {batchSize: 0}};
    for (let opt of Object.keys(options)) {
        cmd[opt] = options[opt];
    }

    let resultObj = coll.runCommand("aggregate", cmd);
    if (resultObj.ok) {
        let followupBatchSize = 0;  // default
        let cursor = new DBCommandCursor(coll.getDB(), resultObj, followupBatchSize);
        let assertThrowsMsg = "expected one of the following error codes: " + tojson(code);
        resultObj = assert.throws(() => cursor.itcount(), [], assertThrowsMsg);
    }

    assert.commandFailedWithCode(resultObj, code, errmsg);
}

/**
 * Assert that an aggregation fails with a specific code and the error message contains the given
 * string. Note that 'code' can be an array of possible codes.
 */
function assertErrCodeAndErrMsgContains(coll, pipe, code, expectedMessage) {
    const response = assert.commandFailedWithCode(
        coll.getDB().runCommand({aggregate: coll.getName(), pipeline: pipe, cursor: {}}), code);
    assert.neq(
        -1,
        response.errmsg.indexOf(expectedMessage),
        "Error message did not contain '" + expectedMessage + "', found:\n" + tojson(response));
}

/**
 * Assert that an aggregation fails with any code and the error message contains the given
 * string.
 */
function assertErrMsgContains(coll, pipe, expectedMessage) {
    const response = assert.commandFailed(
        coll.getDB().runCommand({aggregate: coll.getName(), pipeline: pipe, cursor: {}}));
    assert.neq(
        -1,
        response.errmsg.indexOf(expectedMessage),
        "Error message did not contain '" + expectedMessage + "', found:\n" + tojson(response));
}

/**
 * Assert that an aggregation fails with any code and the error message does not contain the given
 * string.
 */
function assertErrMsgDoesNotContain(coll, pipe, expectedMessage) {
    const response = assert.commandFailed(
        coll.getDB().runCommand({aggregate: coll.getName(), pipeline: pipe, cursor: {}}));
    assert.eq(-1,
              response.errmsg.indexOf(expectedMessage),
              "Error message contained '" + expectedMessage + "'");
}

/**
 * Asserts that two arrays are equal - that is, if their sizes are equal and each element in
 * the 'actual' array has a matching element in the 'expected' array, without honoring elements
 * order.
 */
function assertArrayEq({actual = [], expected = [], fieldsToSkip = []} = {}) {
    assert(arrayEq(actual, expected, false, null, fieldsToSkip),
           `actual=${tojson(actual)}, expected=${tojson(expected)}`);
}

/**
 * Generates the 'numDocs' number of documents each of 'docSize' size and inserts them into the
 * collecton 'coll'. Each document is generated from the 'template' function, which, by default,
 * returns a document in the form of {_id: i}, where 'i' is the iteration index, starting from 0.
 * The 'template' function is called on each iteration and can take three arguments and return
 * any JSON document which will be used as a document template:
 *   - 'itNum' - the current iteration number in the range [0, numDocs)
 *   - 'docSize' - is the 'docSize' parameter passed to 'generateCollection'
 *   - 'numDocs' - is the 'numDocs' parameter passed to 'generateCollection'
 *
 * After a document is generated from the template, it will be assigned a new field called 'padding'
 * holding a repeating string of 'x' characters, so that the total size of the generated object
 * equals to 'docSize'.
 */
function generateCollection({
    coll = null,
    numDocs = 0,
    docSize = 0,
    template =
        (itNum) => {
            return {_id: itNum};
        }
} = {}) {
    assert(coll, "Collection not provided");

    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; ++i) {
        const doc = Object.assign({padding: ""}, template(i, docSize, numDocs));
        const len = docSize - Object.bsonsize(doc);
        assert.lte(0, len, `Document is already bigger than ${docSize} bytes: ${tojson(doc)}`);

        doc.padding = "x".repeat(len);
        assert.eq(
            docSize,
            Object.bsonsize(doc),
            `Generated document's size doesn't match requested document's size: ${tojson(doc)}`);

        bulk.insert(doc);
    }

    const res = bulk.execute();
    assert.commandWorked(res);
    assert.eq(numDocs, res.nInserted);
    assert.eq(numDocs, coll.find().itcount());
}

/**
 * Returns true if 'coll' exists or false otherwise.
 */
function collectionExists(coll) {
    return Array.contains(coll.getDB().getCollectionNames(), coll.getName());
}
