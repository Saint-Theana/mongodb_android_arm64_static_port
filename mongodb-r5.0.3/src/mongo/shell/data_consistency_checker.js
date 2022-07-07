var CollInfos = class {
    /**
     * OO wrapper around the response of db.getCollectionInfos() to avoid calling it multiple times.
     * This class stores information about all collections but its methods typically apply to a
     * single collection, so a collName is typically required to be passed in as a parameter.
     */
    constructor(conn, connName, dbName) {
        // Special listCollections filter to prevent reloading the view catalog.
        const listCollectionsFilter = {
            $or: [
                {type: 'collection'},
                {type: {$exists: false}},
            ]
        };
        this.conn = conn;
        this.connName = connName;
        this.dbName = dbName;
        this.collInfosRes = conn.getDB(dbName).getCollectionInfos(listCollectionsFilter);
    }

    ns(collName) {
        return this.dbName + '.' + collName;
    }

    /**
     * Do additional filtering to narrow down collections that have names in collNames.
     */
    filter(desiredCollNames) {
        this.collInfosRes = this.collInfosRes.filter(info => desiredCollNames.includes(info.name));
    }

    /**
     * Get collInfo for non-capped collections.
     *
     * Don't call isCapped(), which calls listCollections.
     */
    getNonCappedCollNames() {
        const infos = this.collInfosRes.filter(info => !info.options.capped);
        return infos.map(info => info.name);
    }

    hostAndNS(collName) {
        return `${this.conn.host}--${this.ns(collName)}`;
    }

    print(collectionPrinted, collName) {
        const ns = this.ns(collName);
        const alreadyPrinted = collectionPrinted.has(this.hostAndNS(collName));

        // Extract basic collection info.
        const coll = this.conn.getDB(this.dbName).getCollection(collName);
        let collInfo = null;

        const collInfoRaw = this.collInfosRes.find(elem => elem.name === collName);
        if (collInfoRaw) {
            collInfo = {
                ns: ns,
                host: this.conn.host,
                UUID: collInfoRaw.info.uuid,
                count: coll.find().itcount(),
                raw: collInfoRaw,
            };
        }

        const infoPrefix = `${this.connName}(${this.conn.host}) info for ${ns} : `;
        if (collInfo !== null) {
            if (alreadyPrinted) {
                print(`${this.connName} info for ${ns} already printed. Search for ` +
                      `'${infoPrefix}'`);
            } else {
                print(infoPrefix + tojsononeline(collInfo));
            }
        } else {
            print(infoPrefix + 'collection does not exist');
        }

        const collStats = this.conn.getDB(this.dbName).runCommand({collStats: collName});
        const statsPrefix = `${this.connName}(${this.conn.host}) collStats for ${ns}: `;
        if (collStats.ok === 1) {
            if (alreadyPrinted) {
                print(`${this.connName} collStats for ${ns} already printed. Search for ` +
                      `'${statsPrefix}'`);
            } else {
                print(statsPrefix + tojsononeline(collStats));
            }
        } else {
            print(`${statsPrefix}  error: ${tojsononeline(collStats)}`);
        }

        collectionPrinted.add(this.hostAndNS(collName));

        // Return true if collInfo & collStats can be retrieved for conn.
        return collInfo !== null && collStats.ok === 1;
    }
};

var {DataConsistencyChecker} = (function() {
    "use strict";

    class PeekableCursor {
        constructor(cursor) {
            this.cursor = cursor;
            this.stashedDoc = undefined;
        }

        hasNext() {
            return this.cursor.hasNext();
        }

        peekNext() {
            if (this.stashedDoc === undefined) {
                this.stashedDoc = this.cursor.next();
            }
            return this.stashedDoc;
        }

        next() {
            const result = (this.stashedDoc === undefined) ? this.cursor.next() : this.stashedDoc;
            this.stashedDoc = undefined;
            return result;
        }
    }

    class DataConsistencyChecker {
        static getDiff(cursor1, cursor2) {
            const docsWithDifferentContents = [];
            const docsMissingOnFirst = [];
            const docsMissingOnSecond = [];

            cursor1 = new PeekableCursor(cursor1);
            cursor2 = new PeekableCursor(cursor2);

            while (cursor1.hasNext() && cursor2.hasNext()) {
                const doc1 = cursor1.peekNext();
                const doc2 = cursor2.peekNext();

                if (bsonBinaryEqual(doc1, doc2)) {
                    // The same document was found from both cursor1 and cursor2 so we just move
                    // on to the next document for both cursors.
                    cursor1.next();
                    cursor2.next();
                    continue;
                }

                const ordering = bsonWoCompare({_: doc1._id}, {_: doc2._id});
                if (ordering === 0) {
                    // The documents have the same _id but have different contents.
                    docsWithDifferentContents.push({first: doc1, second: doc2});
                    cursor1.next();
                    cursor2.next();
                } else if (ordering < 0) {
                    // The cursor1's next document has a smaller _id than the cursor2's next
                    // document. Since we are iterating the documents in ascending order by their
                    // _id, we'll never see a document with 'doc1._id' from cursor2.
                    docsMissingOnSecond.push(doc1);
                    cursor1.next();
                } else if (ordering > 0) {
                    // The cursor1's next document has a larger _id than the cursor2's next
                    // document. Since we are iterating the documents in ascending order by their
                    // _id, we'll never see a document with 'doc2._id' from cursor1.
                    docsMissingOnFirst.push(doc2);
                    cursor2.next();
                }
            }

            while (cursor1.hasNext()) {
                // We've exhausted cursor2 already, so everything remaining from cursor1 must be
                // missing from cursor2.
                docsMissingOnSecond.push(cursor1.next());
            }

            while (cursor2.hasNext()) {
                // We've exhausted cursor1 already, so everything remaining from cursor2 must be
                // missing from cursor1.
                docsMissingOnFirst.push(cursor2.next());
            }

            return {docsWithDifferentContents, docsMissingOnFirst, docsMissingOnSecond};
        }

        static getCollectionDiffUsingSessions(
            sourceSession, syncingSession, dbName, collNameOrUUID, readAtClusterTime) {
            const sourceDB = sourceSession.getDatabase(dbName);
            const syncingDB = syncingSession.getDatabase(dbName);

            const commandObj = {find: collNameOrUUID, sort: {_id: 1}};
            if (readAtClusterTime !== undefined) {
                commandObj.readConcern = {level: "snapshot", atClusterTime: readAtClusterTime};
            }

            const sourceCursor = new DBCommandCursor(sourceDB, sourceDB.runCommand(commandObj));
            const syncingCursor = new DBCommandCursor(syncingDB, syncingDB.runCommand(commandObj));
            const diff = this.getDiff(sourceCursor, syncingCursor);

            return {
                docsWithDifferentContents: diff.docsWithDifferentContents.map(
                    ({first, second}) => ({sourceNode: first, syncingNode: second})),
                docsMissingOnSource: diff.docsMissingOnFirst,
                docsMissingOnSyncing: diff.docsMissingOnSecond
            };
        }

        static dumpCollectionDiff(collectionPrinted, sourceCollInfos, syncingCollInfos, collName) {
            print('Dumping collection: ' + sourceCollInfos.ns(collName));

            const sourceExists = sourceCollInfos.print(collectionPrinted, collName);
            const syncingExists = syncingCollInfos.print(collectionPrinted, collName);

            if (!sourceExists || !syncingExists) {
                print(`Skipping checking collection differences for ${
                    sourceCollInfos.ns(collName)} since it does not exist on both nodes`);
                return;
            }

            const sourceNode = sourceCollInfos.conn;
            const syncingNode = syncingCollInfos.conn;

            const sourceSession = sourceNode.getDB('test').getSession();
            const syncingSession = syncingNode.getDB('test').getSession();
            const diff = this.getCollectionDiffUsingSessions(
                sourceSession, syncingSession, sourceCollInfos.dbName, collName);

            for (let {
                     sourceNode: sourceDoc,
                     syncingNode: syncingDoc,
                 } of diff.docsWithDifferentContents) {
                print(`Mismatching documents between the source node ${sourceNode.host}` +
                      ` and the syncing node ${syncingNode.host}:`);
                print('    sourceNode:   ' + tojsononeline(sourceDoc));
                print('    syncingNode: ' + tojsononeline(syncingDoc));
            }

            if (diff.docsMissingOnSource.length > 0) {
                print(`The following documents are missing on the source node ${sourceNode.host}:`);
                print(diff.docsMissingOnSource.map(doc => tojsononeline(doc)).join('\n'));
            }

            if (diff.docsMissingOnSyncing.length > 0) {
                print(
                    `The following documents are missing on the syncing node ${syncingNode.host}:`);
                print(diff.docsMissingOnSyncing.map(doc => tojsononeline(doc)).join('\n'));
            }
        }

        static checkDBHash(sourceDBHash,
                           sourceCollInfos,
                           syncingDBHash,
                           syncingCollInfos,
                           msgPrefix,
                           ignoreUUIDs,
                           syncingHasIndexes,
                           collectionPrinted) {
            let success = true;

            const sourceDBName = sourceCollInfos.dbName;
            const syncingDBName = syncingCollInfos.dbName;
            assert.eq(
                sourceDBName,
                syncingDBName,
                `dbName was not the same: source: ${sourceDBName}, syncing: ${syncingDBName}`);
            const dbName = syncingDBName;

            const sourceCollections = Object.keys(sourceDBHash.collections);
            const syncingCollections = Object.keys(syncingDBHash.collections);

            const dbHashesMsg =
                `source: ${tojson(sourceDBHash)}, syncing: ${tojson(syncingDBHash)}`;
            const prettyPrint = (outputMsg => {
                print(`${msgPrefix}, ${outputMsg}`);
            });

            const arraySymmetricDifference = ((a, b) => {
                const inAOnly = a.filter(function(elem) {
                    return b.indexOf(elem) < 0;
                });

                const inBOnly = b.filter(function(elem) {
                    return a.indexOf(elem) < 0;
                });

                return inAOnly.concat(inBOnly);
            });

            if (sourceCollections.length !== syncingCollections.length) {
                prettyPrint(`the two nodes have a different number of collections: ${dbHashesMsg}`);
                for (const diffColl of arraySymmetricDifference(sourceCollections,
                                                                syncingCollections)) {
                    this.dumpCollectionDiff(
                        collectionPrinted, sourceCollInfos, syncingCollInfos, diffColl);
                }
                success = false;
            }

            const nonCappedCollNames = sourceCollInfos.getNonCappedCollNames();
            // Only compare the dbhashes of non-capped collections because capped
            // collections are not necessarily truncated at the same points between the source and
            // syncing nodes.
            nonCappedCollNames.forEach(collName => {
                if (sourceDBHash.collections[collName] !== syncingDBHash.collections[collName]) {
                    prettyPrint(`the two nodes have a different hash for the collection ${dbName}.${
                        collName}: ${dbHashesMsg}`);
                    this.dumpCollectionDiff(
                        collectionPrinted, sourceCollInfos, syncingCollInfos, collName);
                    success = false;
                }
            });

            syncingCollInfos.collInfosRes.forEach(syncingInfo => {
                sourceCollInfos.collInfosRes.forEach(sourceInfo => {
                    if (syncingInfo.name === sourceInfo.name &&
                        syncingInfo.type === sourceInfo.type) {
                        if (ignoreUUIDs) {
                            prettyPrint(`skipping UUID check for ${[sourceInfo.name]}`);
                            sourceInfo.info.uuid = null;
                            syncingInfo.info.uuid = null;
                        }

                        // Ignore the 'flags' collection option as it was removed in 4.2
                        sourceInfo.options.flags = null;
                        syncingInfo.options.flags = null;

                        // Ignore the 'ns' field in the 'idIndex' field as 'ns' was removed
                        // from index specs in 4.4.
                        if (sourceInfo.idIndex) {
                            delete sourceInfo.idIndex.ns;
                        }
                        if (syncingInfo.idIndex) {
                            delete syncingInfo.idIndex.ns;
                        }

                        // TODO: SERVER-54967 Remove workaround for comparing size
                        let sizeDeleted = false;
                        let sourceSize = sourceInfo.options.size;
                        let syncingSize = syncingInfo.options.size;

                        // Compare 'size' field in 'options' field outside of bsonBinaryEqual as it
                        // could be saved as a NumberDecimal or NumberLong in versions 4.4 and
                        // before.
                        if (jsTest.options().useRandomBinVersionsWithinReplicaSet &&
                            sourceInfo.options.size == syncingInfo.options.size &&
                            sourceInfo.options.size !== syncingInfo.options.size) {
                            delete sourceInfo.options.size;
                            delete syncingInfo.options.size;
                            sizeDeleted = true;
                        }

                        if (!bsonBinaryEqual(syncingInfo, sourceInfo)) {
                            prettyPrint(
                                `the two nodes have different attributes for the collection or view ${
                                    dbName}.${syncingInfo.name}`);
                            this.dumpCollectionDiff(collectionPrinted,
                                                    sourceCollInfos,
                                                    syncingCollInfos,
                                                    syncingInfo.name);
                            success = false;
                        }

                        // Deleted sizes must be added back to prevent comparison between nodes that
                        // have not had their size removed.
                        if (sizeDeleted) {
                            sourceInfo.options.size = sourceSize;
                            syncingInfo.options.size = syncingSize;
                        }
                    }
                });
            });

            // Treats each array as a set and returns true if the contents match. Assumes
            // the contents of each array are unique.
            const compareSets = function(leftArr, rightArr) {
                if (leftArr === undefined) {
                    return rightArr === undefined;
                }

                if (rightArr === undefined) {
                    return false;
                }

                const map = {};
                leftArr.forEach(key => {
                    map[key] = 1;
                });

                rightArr.forEach(key => {
                    if (map[key] === undefined) {
                        map[key] = -1;
                    } else {
                        delete map[key];
                    }
                });

                // The map is empty when both sets match.
                for (let key in map) {
                    if (map.hasOwnProperty(key)) {
                        return false;
                    }
                }
                return true;
            };

            const sourceNode = sourceCollInfos.conn;
            const syncingNode = syncingCollInfos.conn;

            // Check that the following collection stats are the same between the source and syncing
            // nodes:
            //  capped
            //  nindexes, except on nodes with buildIndexes: false
            //  ns
            sourceCollections.forEach(collName => {
                const sourceCollStats = sourceNode.getDB(dbName).runCommand({collStats: collName});
                const syncingCollStats =
                    syncingNode.getDB(dbName).runCommand({collStats: collName});

                if (sourceCollStats.ok !== 1 || syncingCollStats.ok !== 1) {
                    sourceCollInfos.print(collectionPrinted, collName);
                    syncingCollInfos.print(collectionPrinted, collName);
                    success = false;
                    return;
                }

                // Provide hint on where to look within stats.
                let reasons = [];
                if (sourceCollStats.capped !== syncingCollStats.capped) {
                    reasons.push('capped');
                }

                if (sourceCollStats.ns !== syncingCollStats.ns) {
                    reasons.push('ns');
                }

                if (syncingHasIndexes && sourceCollStats.nindexes !== syncingCollStats.nindexes) {
                    reasons.push('indexes');
                }

                const indexBuildsMatch =
                    compareSets(sourceCollStats.indexBuilds, syncingCollStats.indexBuilds);
                if (syncingHasIndexes && !indexBuildsMatch) {
                    reasons.push('indexBuilds');
                }

                if (reasons.length === 0) {
                    return;
                }

                prettyPrint(`the two nodes have different states for the collection ${dbName}.${
                    collName}: ${reasons.join(', ')}`);
                this.dumpCollectionDiff(
                    collectionPrinted, sourceCollInfos, syncingCollInfos, collName);
                success = false;
            });

            if (nonCappedCollNames.length === sourceCollections.length) {
                // If the two nodes have the same hashes for all the
                // collections in the database and there aren't any capped collections,
                // then the hashes for the whole database should match.
                if (sourceDBHash.md5 !== syncingDBHash.md5) {
                    prettyPrint(`the two nodes have a different has for the ${dbName} database: ${
                        dbHashesMsg}`);
                    success = false;
                }
            }

            return success;
        }
    }

    return {DataConsistencyChecker};
})();
