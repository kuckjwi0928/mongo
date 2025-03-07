/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/cqf_get_executor.h"

#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <absl/meta/type_traits.h>
// IWYU pragma: no_include "boost/container/detail/flat_tree.hpp"
#include <boost/container/vector.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/ordering.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/exec/sbe/abt/abt_lower_defs.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/shard_filterer_impl.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/abt/canonical_query_translation.h"
#include "mongo/db/pipeline/abt/document_source_visitor.h"
#include "mongo/db/pipeline/abt/match_expression_visitor.h"
#include "mongo/db/pipeline/abt/utils.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/bind_input_params.h"
#include "mongo/db/query/ce/heuristic_estimator.h"
#include "mongo/db/query/ce/histogram_estimator.h"
#include "mongo/db/query/ce/sampling_estimator.h"
#include "mongo/db/query/ce/sampling_executor.h"
#include "mongo/db/query/ce_mode_parameter.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/cost_model/cost_estimator_impl.h"
#include "mongo/db/query/cost_model/cost_model_gen.h"
#include "mongo/db/query/cost_model/cost_model_manager.h"
#include "mongo/db/query/cost_model/on_coefficients_change_updater_impl.h"
#include "mongo/db/query/cqf_command_utils.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/optimizer/cascades/interfaces.h"
#include "mongo/db/query/optimizer/cascades/memo.h"
#include "mongo/db/query/optimizer/containers.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/metadata_factory.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/node_defs.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/partial_schema_requirements.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/path.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/const_fold_interface.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_decorations.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/shard_filterer_factory_impl.h"
#include "mongo/db/query/stats/collection_statistics_impl.h"
#include "mongo/db/query/yield_policy_callbacks_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryOptimizer

MONGO_FAIL_POINT_DEFINE(failConstructingBonsaiExecutor);

namespace mongo {
using namespace optimizer;
using ce::HeuristicEstimator;
using ce::HistogramEstimator;
using ce::SamplingEstimator;
using cost_model::CostEstimatorImpl;
using cost_model::CostModelManager;

static std::pair<IndexDefinitions, MultikeynessTrie> buildIndexSpecsOptimizer(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const boost::optional<BSONObj>& indexHint,
    const optimizer::ProjectionName& scanProjName,
    PrefixId& prefixId,
    const DisableIndexOptions disableIndexOptions,
    bool& disableScan) {
    using namespace optimizer;

    if (disableIndexOptions == DisableIndexOptions::DisableAll) {
        return {};
    }

    std::pair<IndexDefinitions, MultikeynessTrie> result;
    std::string indexHintName;

    // True if the query has a $natural hint, indicating that we must use a collection scan.
    bool hasNaturalHint = false;

    if (indexHint) {
        const BSONElement element = indexHint->firstElement();
        const StringData fieldName = element.fieldNameStringData();
        if (fieldName == query_request_helper::kNaturalSortField) {
            // Do not add indexes.
            hasNaturalHint = true;
        } else if (fieldName == "$hint"_sd && element.type() == BSONType::String) {
            indexHintName = element.valueStringData().toString();
        }

        disableScan = !hasNaturalHint;
    }

    const IndexCatalog& indexCatalog = *collection->getIndexCatalog();

    auto indexIterator =
        indexCatalog.getIndexIterator(opCtx, IndexCatalog::InclusionPolicy::kReady);

    while (indexIterator->more()) {
        const IndexCatalogEntry& catalogEntry = *indexIterator->next();
        const IndexDescriptor& descriptor = *catalogEntry.descriptor();
        bool skipIndex = false;

        if (descriptor.hidden()) {
            // Index is hidden; don't consider it.
            continue;
        }

        // We support the presence of hashed id indexes in CQF. However. we don't add them to the
        // optimizer metadata as we don't know how to generate plans that use them yet.
        if (descriptor.isHashedIdIndex()) {
            continue;
        }

        // Check for special indexes. We do not want to try to build index metadata for a special
        // index (since we do not support those yet in CQF) but we should allow the query to go
        // through CQF if there is a $natural hint.
        if (descriptor.infoObj().hasField(IndexDescriptor::kExpireAfterSecondsFieldName) ||
            descriptor.isSparse() || descriptor.getIndexType() != IndexType::INDEX_BTREE ||
            !descriptor.collation().isEmpty()) {
            uassert(
                ErrorCodes::InternalErrorNotSupported, "Unsupported index type", hasNaturalHint);
            continue;
        }

        if (indexHint) {
            if (indexHintName.empty()) {
                // Index hint is a key pattern. Check if it matches this descriptor's key pattern.
                skipIndex = SimpleBSONObjComparator::kInstance.evaluate(descriptor.keyPattern() !=
                                                                        *indexHint);
            } else {
                // Index hint is an index name. Check if it matches the descriptor's name.
                skipIndex = indexHintName != descriptor.indexName();
            }
        }

        const bool isMultiKey = catalogEntry.isMultikey(opCtx, collection);
        const MultikeyPaths& multiKeyPaths = catalogEntry.getMultikeyPaths(opCtx, collection);
        uassert(6624251, "Multikey paths cannot be empty.", !multiKeyPaths.empty());

        // SBE version is base 0.
        const int64_t version = static_cast<int>(descriptor.version()) - 1;

        uint32_t orderingBits = 0;
        {
            const Ordering ordering = catalogEntry.ordering();
            for (int i = 0; i < descriptor.getNumFields(); i++) {
                if ((ordering.get(i) == -1)) {
                    orderingBits |= (1ull << i);
                }
            }
        }

        IndexCollationSpec indexCollationSpec;
        bool useIndex = true;
        size_t elementIdx = 0;
        for (const auto& element : descriptor.keyPattern()) {
            FieldPathType fieldPath;
            FieldPath path(element.fieldName());

            for (size_t i = 0; i < path.getPathLength(); i++) {
                const std::string& fieldName = path.getFieldName(i).toString();
                if (fieldName == "$**") {
                    // TODO SERVER-70309: Support wildcard indexes.
                    useIndex = false;
                    break;
                }
                fieldPath.emplace_back(fieldName);
            }
            if (!useIndex) {
                break;
            }

            const int direction = element.numberInt();
            if (direction != -1 && direction != 1) {
                // Invalid value?
                useIndex = false;
                break;
            }

            const CollationOp collationOp =
                (direction == 1) ? CollationOp::Ascending : CollationOp::Descending;

            // Construct an ABT path for each index component (field path).
            const MultikeyComponents& elementMultiKeyInfo = multiKeyPaths[elementIdx];
            ABT abtPath = make<PathIdentity>();
            for (size_t i = fieldPath.size(); i-- > 0;) {
                if (isMultiKey && elementMultiKeyInfo.find(i) != elementMultiKeyInfo.cend()) {
                    // This is a multikey element of the path.
                    abtPath = make<PathTraverse>(PathTraverse::kSingleLevel, std::move(abtPath));
                }
                abtPath = make<PathGet>(fieldPath.at(i), std::move(abtPath));
            }
            indexCollationSpec.emplace_back(std::move(abtPath), collationOp);
            ++elementIdx;
        }
        if (!useIndex) {
            continue;
        }

        PSRExpr::Node partialIndexReqMap = psr::makeNoOp();
        if (descriptor.isPartial() &&
            disableIndexOptions != DisableIndexOptions::DisablePartialOnly) {
            auto expr = MatchExpressionParser::parseAndNormalize(
                descriptor.partialFilterExpression(),
                expCtx,
                ExtensionsCallbackNoop(),
                MatchExpressionParser::kBanAllSpecialFeatures);

            // We need a non-empty root projection name.
            QueryParameterMap qp;
            ABT exprABT = generateMatchExpression(expr.get(),
                                                  false /*allowAggExpression*/,
                                                  "<root>" /*rootProjection*/,
                                                  prefixId,
                                                  qp);
            exprABT = make<EvalFilter>(std::move(exprABT), make<Variable>(scanProjName));

            // TODO SERVER-70315: simplify partial filter expression.
            auto conversion = convertExprToPartialSchemaReq(
                exprABT, true /*isFilterContext*/, {} /*pathToIntervalFn*/);
            if (!conversion) {
                // TODO SERVER-70315: should this conversion be always possible?
                continue;
            }
            tassert(6624257,
                    "Should not be seeing a partial index filter where we need to over-approximate",
                    !conversion->_retainPredicate);

            partialIndexReqMap = std::move(conversion->_reqMap);
        }

        IndexDefinition indexDef(std::move(indexCollationSpec),
                                 version,
                                 orderingBits,
                                 isMultiKey,
                                 DistributionType::Centralized,
                                 std::move(partialIndexReqMap));
        // Skip partial indexes. A path could be non-multikey on a partial index (subset of the
        // collection), but still be multikey on the overall collection.
        if (psr::isNoop(indexDef.getPartialReqMap())) {
            for (const auto& component : indexDef.getCollationSpec()) {
                result.second.add(component._path);
            }
        }
        // For now we assume distribution is Centralized.
        if (!skipIndex && !hasNaturalHint) {
            result.first.emplace(descriptor.indexName(), std::move(indexDef));
        }
    }

    // The empty path refers to the whole document, which can't be an array.
    result.second.isMultiKey = false;

    return result;
}

QueryHints getHintsFromQueryKnobs() {
    QueryHints hints;

    hints._disableScan = internalCascadesOptimizerDisableScan.load();
    hints._disableIndexes = internalCascadesOptimizerDisableIndexes.load()
        ? DisableIndexOptions::DisableAll
        : DisableIndexOptions::Enabled;
    hints._disableHashJoinRIDIntersect =
        internalCascadesOptimizerDisableHashJoinRIDIntersect.load();
    hints._disableMergeJoinRIDIntersect =
        internalCascadesOptimizerDisableMergeJoinRIDIntersect.load();
    hints._disableGroupByAndUnionRIDIntersect =
        internalCascadesOptimizerDisableGroupByAndUnionRIDIntersect.load();
    hints._keepRejectedPlans = internalCascadesOptimizerKeepRejectedPlans.load();
    hints._disableBranchAndBound = internalCascadesOptimizerDisableBranchAndBound.load();
    hints._fastIndexNullHandling = internalCascadesOptimizerFastIndexNullHandling.load();
    hints._disableYieldingTolerantPlans =
        internalCascadesOptimizerDisableYieldingTolerantPlans.load();
    hints._minIndexEqPrefixes = internalCascadesOptimizerMinIndexEqPrefixes.load();
    hints._maxIndexEqPrefixes = internalCascadesOptimizerMaxIndexEqPrefixes.load();
    hints._numSamplingChunks = internalCascadesOptimizerSampleChunks.load();
    hints._enableNotPushdown = internalCascadesOptimizerEnableNotPushdown.load();
    hints._forceSamplingCEFallBackForFilterNode =
        internalCascadesOptimizerSamplingCEFallBackForFilterNode.load();
    hints._samplingCollectionSizeMin = internalCascadesOptimizerSampleSizeMin.load();
    hints._samplingCollectionSizeMax = internalCascadesOptimizerSampleSizeMax.load();
    hints._sqrtSampleSizeEnabled = internalCascadesOptimizerEnableSqrtSampleSize.load();

    return hints;
}

namespace {
/*
 * This function initializes the slot in the SBE runtime environment that provides a
 * 'ShardFilterer' and populates it.
 */
void setupShardFiltering(OperationContext* opCtx,
                         const MultipleCollectionAccessor& collections,
                         mongo::sbe::RuntimeEnvironment& runtimeEnv,
                         sbe::value::SlotIdGenerator& slotIdGenerator) {
    bool isSharded = collections.isAcquisition()
        ? collections.getMainAcquisition().getShardingDescription().isSharded()
        : collections.getMainCollection().isSharded_DEPRECATED();
    if (isSharded) {
        // Allocate a global slot for shard filtering and register it in 'runtimeEnv'.
        sbe::value::SlotId shardFiltererSlot = runtimeEnv.registerSlot(
            kshardFiltererSlotName, sbe::value::TypeTags::Nothing, 0, false, &slotIdGenerator);
        populateShardFiltererSlot(opCtx, runtimeEnv, shardFiltererSlot, collections);
    }
}

static ExecParams createExecutor(
    OptPhaseManager phaseManager,
    PlanAndProps planAndProps,
    OperationContext* opCtx,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const NamespaceString& nss,
    const MultipleCollectionAccessor& collections,
    const bool requireRID,
    const boost::optional<MatchExpression*> pipelineMatchExpr,
    PlanYieldPolicy::YieldPolicy yieldPolicy = PlanYieldPolicy::YieldPolicy::YIELD_AUTO) {
    auto env = VariableEnvironment::build(planAndProps._node);
    SlotVarMap slotMap;
    auto runtimeEnvironment = std::make_unique<sbe::RuntimeEnvironment>();  // TODO use factory
    sbe::value::SlotIdGenerator ids;
    boost::optional<sbe::value::SlotId> ridSlot;

    std::unique_ptr<PlanYieldPolicySBE> sbeYieldPolicy;
    if (!phaseManager.getMetadata().isParallelExecution()) {
        // TODO SERVER-80311: Enable yielding for parallel scan plans.
        sbeYieldPolicy = PlanYieldPolicySBE::make(opCtx, yieldPolicy, collections, nss);
    }

    // Construct the ShardFilterer and bind it to the correct slot.
    setupShardFiltering(opCtx, collections, *runtimeEnvironment, ids);
    auto staticData = std::make_unique<stage_builder::PlanStageStaticData>();

    SBENodeLowering g{env,
                      *runtimeEnvironment,
                      ids,
                      staticData->inputParamToSlotMap,
                      phaseManager.getMetadata(),
                      planAndProps._map,
                      sbeYieldPolicy.get()};
    auto sbePlan = g.optimize(planAndProps._node, slotMap, ridSlot);
    tassert(6624262, "Unexpected rid slot", !requireRID || ridSlot);

    uassert(6624253, "Lowering failed: did not produce a plan.", sbePlan != nullptr);
    uassert(6624254, "Lowering failed: did not produce any output slots.", !slotMap.empty());

    {
        sbe::DebugPrinter p;
        OPTIMIZER_DEBUG_LOG(6264802, 5, "Lowered SBE plan", "plan"_attr = p.print(*sbePlan.get()));
    }

    staticData->resultSlot = slotMap.begin()->second;
    if (requireRID) {
        staticData->recordIdSlot = ridSlot;
    }

    stage_builder::PlanStageData data(stage_builder::Environment(std::move(runtimeEnvironment)),
                                      std::move(staticData));

    sbePlan->attachToOperationContext(opCtx);
    if (expCtx->explain || expCtx->mayDbProfile) {
        sbePlan->markShouldCollectTimingInfo();
    }

    std::unique_ptr<ABTPrinter> abtPrinter;

    // By default, we print the optimized ABT. For test-only versions we output the post-memo
    // plan instead.
    PlanAndProps toExplain = std::move(planAndProps);

    // TODO SERVER-82709: Instead of using the framework control here, use the query eligibility
    // information.
    auto frameworkControl =
        QueryKnobConfiguration::decoration(opCtx).getInternalQueryFrameworkControlForOp();

    ExplainVersion explainVersion = ExplainVersion::Vmax;
    const auto& explainVersionStr = internalCascadesOptimizerExplainVersion.get();
    if (explainVersionStr == "v1"_sd) {
        explainVersion = ExplainVersion::V1;
        toExplain = *phaseManager.getPostMemoPlan();
    } else if (explainVersionStr == "v2"_sd) {
        explainVersion = ExplainVersion::V2;
        toExplain = *phaseManager.getPostMemoPlan();
    } else if (explainVersionStr == "v2compact"_sd) {
        explainVersion = ExplainVersion::V2Compact;
        toExplain = *phaseManager.getPostMemoPlan();
    } else if (explainVersionStr == "bson"_sd &&
               frameworkControl == QueryFrameworkControlEnum::kTryBonsai) {
        explainVersion = ExplainVersion::UserFacingExplain;
    } else if (explainVersionStr == "bson"_sd) {
        explainVersion = ExplainVersion::V3;
    } else {
        // Should have been validated.
        MONGO_UNREACHABLE;
    }

    abtPrinter = std::make_unique<ABTPrinter>(
        phaseManager.getMetadata(), std::move(toExplain), explainVersion);

    sbePlan->prepare(data.env.ctx);
    CurOp::get(opCtx)->stopQueryPlanningTimer();

    return {opCtx,
            nullptr /*solution*/,
            {std::move(sbePlan), std::move(data)},
            std::move(abtPrinter),
            QueryPlannerParams::Options::DEFAULT,
            nss,
            std::move(sbeYieldPolicy),
            false /*isFromPlanCache*/,
            true /* generatedByBonsai */,
            pipelineMatchExpr};
}

}  // namespace

static void populateAdditionalScanDefs(
    OperationContext* opCtx,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const stdx::unordered_set<NamespaceString>& involvedCollections,
    const boost::optional<BSONObj>& indexHint,
    const size_t numberOfPartitions,
    PrefixId& prefixId,
    opt::unordered_map<std::string, ScanDefinition>& scanDefs,
    const ConstFoldFn& constFold,
    const DisableIndexOptions disableIndexOptions,
    bool& disableScan) {
    for (const auto& involvedNss : involvedCollections) {
        // TODO SERVER-70304 Allow queries over views and reconsider locking strategy for
        // multi-collection queries.
        AutoGetCollectionForReadCommandMaybeLockFree ctx(opCtx, involvedNss);
        const CollectionPtr& collection = ctx ? ctx.getCollection() : CollectionPtr::null;
        const bool collectionExists = static_cast<bool>(collection);
        const std::string collNameStr = involvedNss.coll().toString();

        // TODO SERVER-70349: Make this consistent with the base collection scan def name.
        // We cannot add the uuidStr suffix because the pipeline translation does not have
        // access to the metadata so it generates a scan over just the collection name.
        const std::string scanDefName = collNameStr;

        IndexDefinitions indexDefs;
        MultikeynessTrie multikeynessTrie;
        const ProjectionName& scanProjName = prefixId.getNextId("scan");
        if (collectionExists) {
            tie(indexDefs, multikeynessTrie) = buildIndexSpecsOptimizer(expCtx,
                                                                        opCtx,
                                                                        collection,
                                                                        indexHint,
                                                                        scanProjName,
                                                                        prefixId,
                                                                        disableIndexOptions,
                                                                        disableScan);
        }

        // For now handle only local parallelism (no over-the-network exchanges).
        DistributionAndPaths distribution{(numberOfPartitions == 1)
                                              ? DistributionType::Centralized
                                              : DistributionType::UnknownPartitioning};

        boost::optional<CEType> collectionCE;
        if (collectionExists) {
            collectionCE = collection->numRecords(opCtx);
        }

        // We use a forward scan order below by default since these collections are not the main
        // collection of the query (and currently, the scan order can only be non-forward for the
        // main collection).
        scanDefs.emplace(
            scanDefName,
            createScanDef(involvedNss.dbName(),
                          collectionExists ? boost::optional<UUID>{collection->uuid()}
                                           : boost::optional<UUID>{},
                          {{"type", "mongod"}, {ScanNode::kDefaultCollectionNameSpec, collNameStr}},
                          std::move(indexDefs),
                          std::move(multikeynessTrie),
                          constFold,
                          std::move(distribution),
                          collectionExists,
                          collectionCE));
    }
}

// Enforce that unsupported command options don't run through Bonsai. Note these checks are already
// present in the Bonsai fallback mechansim, but those checks are skipped when Bonsai is forced.
// This function prevents us from accidently forcing Bonsai with an unsupported option.
void validateFindCommandOptions(const FindCommandRequest& req) {
    uassert(ErrorCodes::InternalErrorNotSupported,
            "$_requestResumeToken unsupported in CQF",
            !req.getRequestResumeToken());
    uassert(ErrorCodes::InternalErrorNotSupported,
            "allowPartialResults unsupported in CQF",
            !req.getAllowPartialResults());
    uassert(ErrorCodes::InternalErrorNotSupported,
            "allowSpeculativeMajorityRead unsupported in CQF",
            !req.getAllowSpeculativeMajorityRead());
    uassert(
        ErrorCodes::InternalErrorNotSupported, "awaitData unsupported in CQF", !req.getAwaitData());
    uassert(ErrorCodes::InternalErrorNotSupported,
            "collation unsupported in CQF",
            req.getCollation().isEmpty() ||
                SimpleBSONObjComparator::kInstance.evaluate(req.getCollation() ==
                                                            CollationSpec::kSimpleSpec));
    uassert(
        ErrorCodes::InternalErrorNotSupported, "min unsupported in CQF", req.getMin().isEmpty());
    uassert(
        ErrorCodes::InternalErrorNotSupported, "max unsupported in CQF", req.getMax().isEmpty());
    uassert(ErrorCodes::InternalErrorNotSupported,
            "noCursorTimeout unsupported in CQF",
            !req.getNoCursorTimeout());
    uassert(
        ErrorCodes::InternalErrorNotSupported, "readOnce unsupported in CQF", !req.getReadOnce());
    uassert(
        ErrorCodes::InternalErrorNotSupported, "returnKey unsupported in CQF", !req.getReturnKey());
    uassert(ErrorCodes::InternalErrorNotSupported,
            "runtimeConstants unsupported in CQF",
            !req.getLegacyRuntimeConstants());
    uassert(ErrorCodes::InternalErrorNotSupported,
            "showRecordId unsupported in CQF",
            !req.getShowRecordId());
    uassert(
        ErrorCodes::InternalErrorNotSupported, "tailable unsupported in CQF", !req.getTailable());
    uassert(ErrorCodes::InternalErrorNotSupported, "term unsupported in CQF", !req.getTerm());
}

void validateCommandOptions(const CanonicalQuery* query,
                            const CollectionPtr& collection,
                            const boost::optional<BSONObj>& indexHint,
                            const stdx::unordered_set<NamespaceString>& involvedCollections) {
    if (query) {
        validateFindCommandOptions(query->getFindCommandRequest());
    }
    if (indexHint) {
        uassert(6624256,
                "For now we can apply hints only for queries involving a single collection",
                involvedCollections.empty());
        uassert(ErrorCodes::BadValue,
                "$natural hint cannot be set to a value other than -1 or 1.",
                !query_request_helper::hasInvalidNaturalParam(indexHint.value()));
    }
    // Unsupported command/collection options.
    uassert(ErrorCodes::InternalErrorNotSupported,
            "Collection-default collation is not supported",
            !collection || collection->getCollectionOptions().collation.isEmpty());

    uassert(ErrorCodes::InternalErrorNotSupported,
            "Clustered collections are not supported",
            !collection || !collection->isClustered());

    uassert(ErrorCodes::InternalErrorNotSupported,
            "Timeseries collections are not supported",
            !collection || !collection->getTimeseriesOptions());

    uassert(ErrorCodes::InternalErrorNotSupported,
            "Capped collections are not supported",
            !collection || !collection->isCapped());
}

Metadata populateMetadata(boost::intrusive_ptr<ExpressionContext> expCtx,
                          const CollectionPtr& collection,
                          const stdx::unordered_set<NamespaceString>& involvedCollections,
                          const NamespaceString& nss,
                          const boost::optional<BSONObj>& indexHint,
                          const ProjectionName& scanProjName,
                          const std::string& uuidStr,
                          const std::string& scanDefName,
                          const ConstFoldFn& constFold,
                          QueryHints& queryHints,
                          PrefixId& prefixId) {
    auto opCtx = expCtx->opCtx;
    const bool collectionExists = static_cast<bool>(collection);

    // Add the base collection metadata.
    opt::unordered_map<std::string, optimizer::IndexDefinition> indexDefs;
    MultikeynessTrie multikeynessTrie;
    if (collectionExists) {
        tie(indexDefs, multikeynessTrie) = buildIndexSpecsOptimizer(expCtx,
                                                                    opCtx,
                                                                    collection,
                                                                    indexHint,
                                                                    scanProjName,
                                                                    prefixId,
                                                                    queryHints._disableIndexes,
                                                                    queryHints._disableScan);
    }

    const size_t numberOfPartitions = internalQueryDefaultDOP.load();

    const bool isSharded = collection.isSharded_DEPRECATED();
    IndexCollationSpec shardKey;
    if (isSharded) {
        for (auto&& e : collection.getShardKeyPattern().getKeyPattern().toBSON()) {
            CollationOp collationOp{CollationOp::Ascending};
            if (e.type() == BSONType::String && e.String() == IndexNames::HASHED) {
                collationOp = CollationOp::Clustered;
            }
            shardKey.emplace_back(translateShardKeyField(e.fieldName()), collationOp);
        }
    }

    // For now handle only local parallelism (no over-the-network exchanges).
    DistributionAndPaths distribution{(numberOfPartitions == 1)
                                          ? DistributionType::Centralized
                                          : DistributionType::UnknownPartitioning};

    opt::unordered_map<std::string, ScanDefinition> scanDefs;
    boost::optional<CEType> numRecords;
    if (collectionExists) {
        numRecords = static_cast<double>(collection->numRecords(opCtx));
    }
    ShardingMetadata shardingMetadata(shardKey, isSharded);

    auto scanOrder = ScanOrder::Forward;
    if (indexHint && indexHint->firstElementFieldNameStringData() == "$natural"_sd &&
        indexHint->firstElement().safeNumberInt() < 0) {
        scanOrder = ScanOrder::Reverse;
    }

    scanDefs.emplace(
        scanDefName,
        createScanDef(
            nss.dbName(),
            collectionExists ? boost::optional<UUID>{collection->uuid()} : boost::optional<UUID>{},
            {{"type", "mongod"}, {ScanNode::kDefaultCollectionNameSpec, nss.coll().toString()}},
            std::move(indexDefs),
            std::move(multikeynessTrie),
            constFold,
            std::move(distribution),
            collectionExists,
            numRecords,
            std::move(shardingMetadata),
            {} /* indexedFieldPaths*/,
            scanOrder));

    // Add a scan definition for all involved collections. Note that the base namespace has already
    // been accounted for above and isn't included here.
    populateAdditionalScanDefs(opCtx,
                               expCtx,
                               involvedCollections,
                               indexHint,
                               numberOfPartitions,
                               prefixId,
                               scanDefs,
                               constFold,
                               queryHints._disableIndexes,
                               queryHints._disableScan);

    return {std::move(scanDefs), numberOfPartitions};
}

enum class CEMode { kSampling, kHistogram, kHeuristic };

static OptPhaseManager createPhaseManager(const CEMode mode,
                                          const cost_model::CostModelCoefficients& costModel,
                                          const NamespaceString& nss,
                                          OperationContext* opCtx,
                                          const int64_t collectionSize,
                                          PrefixId& prefixId,
                                          const bool requireRID,
                                          Metadata metadata,
                                          const ConstFoldFn& constFold,
                                          QueryHints hints,
                                          QueryParameterMap queryParameters) {
    switch (mode) {
        case CEMode::kSampling: {
            Metadata metadataForSampling = metadata;
            for (auto& entry : metadataForSampling._scanDefs) {
                // Do not use indexes for sampling.
                entry.second.getIndexDefs().clear();

                // Setting the scan order for all scan definitions will cause any PhysicalScanNodes
                // in the tree for that scan to have the appropriate scan order.
                entry.second.setScanOrder(internalCascadesOptimizerSamplingCEScanStartOfColl.load()
                                              ? ScanOrder::Forward
                                              : ScanOrder::Random);

                // Do not perform shard filtering for sampling.
                entry.second.shardingMetadata().setMayContainOrphans(false);
            }


            // For sampling estimator, we do not run constant folding, path fusion and exploration
            // phases.
            OptPhaseManager::PhaseSet rewritesSetForSampling{OptPhase::MemoSubstitutionPhase,
                                                             OptPhase::MemoImplementationPhase,
                                                             OptPhase::PathLower,
                                                             OptPhase::ConstEvalPost_ForSampling};
            OptPhaseManager phaseManagerForSampling{
                std::move(rewritesSetForSampling),
                prefixId,
                false /*requireRID*/,
                std::move(metadataForSampling),
                std::make_unique<HeuristicEstimator>(),
                std::make_unique<HeuristicEstimator>(),
                std::make_unique<CostEstimatorImpl>(costModel),
                defaultConvertPathToInterval,
                constFold,
                DebugInfo::kDefaultForProd,
                {._numSamplingChunks = hints._numSamplingChunks,
                 ._samplingCollectionSizeMin = hints._samplingCollectionSizeMin,
                 ._samplingCollectionSizeMax = hints._samplingCollectionSizeMax,
                 ._sqrtSampleSizeEnabled = hints._sqrtSampleSizeEnabled} /*hints*/,
                queryParameters};

            auto samplingEstimator = std::make_unique<SamplingEstimator>(
                std::move(phaseManagerForSampling),
                collectionSize,
                std::make_unique<HeuristicEstimator>(),
                std::make_unique<ce::SBESamplingExecutor>(opCtx));

            return {OptPhaseManager::getAllProdRewrites(),
                    prefixId,
                    requireRID,
                    std::move(metadata),
                    std::move(samplingEstimator),
                    std::make_unique<HeuristicEstimator>(),
                    std::make_unique<CostEstimatorImpl>(costModel),
                    defaultConvertPathToInterval,
                    constFold,
                    DebugInfo::kDefaultForProd,
                    std::move(hints),
                    std::move(queryParameters)};
        }

        case CEMode::kHistogram:
            return {OptPhaseManager::getAllProdRewrites(),
                    prefixId,
                    requireRID,
                    std::move(metadata),
                    std::make_unique<HistogramEstimator>(
                        std::make_shared<stats::CollectionStatisticsImpl>(collectionSize, nss),
                        std::make_unique<HeuristicEstimator>()),
                    std::make_unique<HeuristicEstimator>(),
                    std::make_unique<CostEstimatorImpl>(costModel),
                    defaultConvertPathToInterval,
                    constFold,
                    DebugInfo::kDefaultForProd,
                    std::move(hints),
                    std::move(queryParameters)};

        case CEMode::kHeuristic:
            return {OptPhaseManager::getAllProdRewrites(),
                    prefixId,
                    requireRID,
                    std::move(metadata),
                    std::make_unique<HeuristicEstimator>(),
                    std::make_unique<HeuristicEstimator>(),
                    std::make_unique<CostEstimatorImpl>(costModel),
                    defaultConvertPathToInterval,
                    constFold,
                    DebugInfo::kDefaultForProd,
                    std::move(hints),
                    std::move(queryParameters)};

        default:
            MONGO_UNREACHABLE;
    }
}

boost::optional<ExecParams> getSBEExecutorViaCascadesOptimizer(
    OperationContext* opCtx,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const NamespaceString& nss,
    const MultipleCollectionAccessor& collections,
    QueryHints queryHints,
    const boost::optional<BSONObj>& indexHint,
    const Pipeline* pipeline,
    const CanonicalQuery* canonicalQuery) {
    if (MONGO_unlikely(failConstructingBonsaiExecutor.shouldFail())) {
        uasserted(620340, "attempting to use CQF while it is disabled");
    }
    // Ensure that either pipeline or canonicalQuery is set.
    tassert(624070,
            "getSBEExecutorViaCascadesOptimizer expects exactly one of the following to be set: "
            "canonicalQuery, pipeline",
            static_cast<bool>(pipeline) != static_cast<bool>(canonicalQuery));

    stdx::unordered_set<NamespaceString> involvedCollections;
    if (pipeline) {
        involvedCollections = pipeline->getInvolvedCollections();
    }

    const auto& collection = collections.getMainCollection();

    const boost::optional<BSONObj>& hint =
        (indexHint && !indexHint->isEmpty() ? indexHint : boost::none);

    validateCommandOptions(canonicalQuery, collection, hint, involvedCollections);

    const bool requireRID = canonicalQuery ? canonicalQuery->getForceGenerateRecordId() : false;
    const bool collectionExists = static_cast<bool>(collection);
    const std::string uuidStr = collectionExists ? collection->uuid().toString() : "<missing_uuid>";
    const std::string collNameStr = nss.coll().toString();
    const std::string scanDefName = collNameStr + "_" + uuidStr;

    // This is the instance we will use to generate variable names during translation and
    // optimization.
    auto prefixId = PrefixId::create(internalCascadesOptimizerUseDescriptiveVarNames.load());
    const ProjectionName& scanProjName = prefixId.getNextId("scan");

    ConstFoldFn constFold = ConstEval::constFold;
    auto metadata = populateMetadata(expCtx,
                                     collection,
                                     involvedCollections,
                                     nss,
                                     hint,
                                     scanProjName,
                                     uuidStr,
                                     scanDefName,
                                     constFold,
                                     queryHints,
                                     prefixId);

    ABT abt = collectionExists
        ? make<ScanNode>(scanProjName, scanDefName)
        : make<ValueScanNode>(ProjectionNameVector{scanProjName},
                              createInitialScanProps(scanProjName, scanDefName));

    QueryParameterMap queryParameters;

    // Check if pipeline is eligible for plan caching.
    // TODO SERVER-83414: Enable histogram CE with parameterization.
    auto _isCacheable = (internalQueryCardinalityEstimatorMode != "histogram"_sd);
    if (pipeline) {
        _isCacheable &= [&]() -> bool {
            auto& sources = pipeline->getSources();
            if (sources.empty())
                return false;

            // First stage must be a DocumentSourceMatch.
            const auto& it = sources.begin();
            auto firstStageName = it->get()->getSourceName();
            if (firstStageName != DocumentSourceMatch::kStageName)
                return false;

            // If optional second stage exists, must be a projection stage.
            auto secondStageItr = std::next(it);
            if (secondStageItr != sources.end())
                return secondStageItr->get()->getSourceName() == DocumentSourceProject::kStageName;

            return true;
        }();
        _isCacheable = false;  // TODO: SERVER-82185: Remove once E2E parameterization enabled
        if (_isCacheable) {
            MatchExpression::parameterize(
                dynamic_cast<DocumentSourceMatch*>(pipeline->peekFront())->getMatchExpression());
        }
        abt = translatePipelineToABT(
            metadata, *pipeline, scanProjName, std::move(abt), prefixId, queryParameters);
    } else {
        // Clear match expression auto-parameterization by setting max param count to zero before
        // CQ to ABT translation
        // TODO: SERVER-82185: Update value of _isCacheable to true for M2-eligible queries
        _isCacheable = false;  // TODO: SERVER-82185: Remove once E2E parameterization enabled
        if (!_isCacheable) {
            MatchExpression::unparameterize(canonicalQuery->getPrimaryMatchExpression());
        }
        abt = translateCanonicalQueryToABT(
            metadata, *canonicalQuery, scanProjName, std::move(abt), prefixId, queryParameters);
    }
    // If pipeline exists and is cacheable, save the MatchExpression in ExecParams for binding.
    const auto pipelineMatchExpr = pipeline && _isCacheable
        ? boost::make_optional(
              dynamic_cast<DocumentSourceMatch*>(pipeline->peekFront())->getMatchExpression())
        : boost::none;

    OPTIMIZER_DEBUG_LOG(
        6264803, 5, "Translated ABT", "explain"_attr = ExplainGenerator::explainV2Compact(abt));

    const int64_t numRecords = collectionExists ? collection->numRecords(opCtx) : -1;
    CEMode mode = CEMode::kHeuristic;

    // TODO: SERVER-70241: Handle "auto" estimation mode.
    if (internalQueryCardinalityEstimatorMode == ce::kSampling) {
        if (collectionExists && numRecords > internalCascadesOptimizerSampleSizeMin.load()) {
            mode = CEMode::kSampling;
        }
    } else if (internalQueryCardinalityEstimatorMode == ce::kHistogram) {
        mode = CEMode::kHistogram;
    } else if (internalQueryCardinalityEstimatorMode == ce::kHeuristic) {
        mode = CEMode::kHeuristic;
    } else {
        tasserted(6624252,
                  str::stream() << "Unknown estimator mode: "
                                << internalQueryCardinalityEstimatorMode);
    }

    auto costModel = cost_model::costModelManager(opCtx->getServiceContext()).getCoefficients();
    OptPhaseManager phaseManager = createPhaseManager(mode,
                                                      costModel,
                                                      nss,
                                                      opCtx,
                                                      numRecords,
                                                      prefixId,
                                                      requireRID,
                                                      std::move(metadata),
                                                      constFold,
                                                      std::move(queryHints),
                                                      std::move(queryParameters));
    auto resultPlans = phaseManager.optimizeNoAssert(std::move(abt), false /*includeRejected*/);
    if (resultPlans.empty()) {
        // Could not find a plan.
        return boost::none;
    }
    // At this point we should have exactly one plan.
    PlanAndProps planAndProps = std::move(resultPlans.front());

    {
        const auto& memo = phaseManager.getMemo();
        const auto& memoStats = memo.getStats();
        if (memoStats._estimatedCost) {
            CurOp::get(opCtx)->debug().estimatedCost = memoStats._estimatedCost->getCost();
        }
        if (memoStats._ce) {
            CurOp::get(opCtx)->debug().estimatedCardinality = (double)*memoStats._ce;
        }
        OPTIMIZER_DEBUG_LOG(
            6264800,
            5,
            "Optimizer stats",
            "memoGroups"_attr = memo.getGroupCount(),
            "memoLogicalNodes"_attr = memo.getLogicalNodeCount(),
            "memoPhysNodes"_attr = memo.getPhysicalNodeCount(),
            "memoIntegrations"_attr = memoStats._numIntegrations,
            "physPlansExplored"_attr = memoStats._physPlanExplorationCount,
            "physMemoChecks"_attr = memoStats._physMemoCheckCount,
            "estimatedCost"_attr =
                (memoStats._estimatedCost ? memoStats._estimatedCost->getCost() : -1.0),
            "estimatedCardinality"_attr = (memoStats._ce ? (double)*memoStats._ce : -1.0));
    }

    const auto explainMemoFn = [&phaseManager]() {
        // Explain the memo only if required by the logging level.
        return ExplainGenerator::explainV2Compact(
            make<MemoPhysicalDelegatorNode>(phaseManager.getPhysicalNodeId()),
            true /*displayPhysicalProperties*/,
            &phaseManager.getMemo());
    };
    OPTIMIZER_DEBUG_LOG(6264801, 5, "Optimized ABT", "explain"_attr = explainMemoFn());

    OPTIMIZER_DEBUG_LOG(6264802,
                        5,
                        "Optimized and lowered physical ABT",
                        "explain"_attr = ExplainGenerator::explainV2(planAndProps._node));

    return createExecutor(std::move(phaseManager),
                          std::move(planAndProps),
                          opCtx,
                          expCtx,
                          nss,
                          collections,
                          requireRID,
                          pipelineMatchExpr);
}

boost::optional<ExecParams> getSBEExecutorViaCascadesOptimizer(
    const MultipleCollectionAccessor& collections,
    QueryHints queryHints,
    const CanonicalQuery* query) {
    boost::optional<BSONObj> indexHint;
    if (!query->getFindCommandRequest().getHint().isEmpty()) {
        indexHint = query->getFindCommandRequest().getHint();
    }

    auto opCtx = query->getOpCtx();
    auto expCtx = query->getExpCtx();
    auto nss = query->nss();

    return getSBEExecutorViaCascadesOptimizer(opCtx,
                                              expCtx,
                                              nss,
                                              collections,
                                              std::move(queryHints),
                                              indexHint,
                                              nullptr /* pipeline */,
                                              query);
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> makeExecFromParams(
    std::unique_ptr<CanonicalQuery> cq,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
    ExecParams execArgs) {
    if (cq) {
        input_params::bind(cq->getPrimaryMatchExpression(), execArgs.root.second, false);
    } else if (execArgs.pipelineMatchExpr != boost::none) {
        // If pipeline contains a parameterized MatchExpression, bind constants.
        input_params::bind(execArgs.pipelineMatchExpr.get(), execArgs.root.second, false);
    }

    return plan_executor_factory::make(execArgs.opCtx,
                                       std::move(cq),
                                       std::move(pipeline),
                                       std::move(execArgs.solution),
                                       std::move(execArgs.root),
                                       std::move(execArgs.optimizerData),
                                       execArgs.plannerOptions,
                                       execArgs.nss,
                                       std::move(execArgs.yieldPolicy),
                                       execArgs.planIsFromCache,
                                       execArgs.generatedByBonsai);
}
}  // namespace mongo
