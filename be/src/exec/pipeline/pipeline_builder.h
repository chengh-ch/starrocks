// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#pragma once

#include "exec/pipeline/exchange/local_exchange.h"
#include "exec/pipeline/exchange/local_exchange_sink_operator.h"
#include "exec/pipeline/exchange/local_exchange_source_operator.h"
#include "exec/pipeline/fragment_context.h"
#include "exec/pipeline/pipeline.h"

namespace starrocks {
class ExecNode;
namespace pipeline {

class PipelineBuilderContext {
public:
    PipelineBuilderContext(FragmentContext* fragment_context, size_t degree_of_parallelism)
            : _fragment_context(fragment_context), _degree_of_parallelism(degree_of_parallelism) {}

    void add_pipeline(const OpFactories& operators) {
        _pipelines.emplace_back(std::make_unique<Pipeline>(next_pipe_id(), operators));
    }

    OpFactories maybe_interpolate_local_broadcast_exchange(OpFactories& pred_operators, int num_receivers);

    // Input the output chunks from the drivers of pred operators into ONE driver of the post operators.
    OpFactories maybe_interpolate_local_passthrough_exchange(OpFactories& pred_operators);
    OpFactories maybe_interpolate_local_passthrough_exchange(OpFactories& pred_operators, int num_receivers);

    // Input the output chunks from multiple drivers of pred operators into DOP drivers of the post operators,
    // by partitioning each row output chunk to DOP partitions according to the key,
    // which is generated by evaluated each row by partition_expr_ctxs.
    // It is used to parallelize complex operators. For example, the build Hash Table (HT) operator can partition
    // the input chunks to build multiple partition HTs, and the probe HT operator can also partition the input chunks
    // and probe on multiple partition HTs in parallel.
    OpFactories maybe_interpolate_local_shuffle_exchange(OpFactories& pred_operators,
                                                         const std::vector<ExprContext*>& partition_expr_ctxs);

    // Uses local exchange to gather the output chunks of multiple predecessor pipelines
    // into a new pipeline, which the successor operator belongs to.
    // Append a LocalExchangeSinkOperator to the tail of each pipeline.
    // Create a new pipeline with a LocalExchangeSourceOperator.
    // These local exchange sink operators and the source operator share a passthrough exchanger.
    OpFactories maybe_gather_pipelines_to_one(std::vector<OpFactories>& pred_operators_list);

    uint32_t next_pipe_id() { return _next_pipeline_id++; }

    uint32_t next_operator_id() { return _next_operator_id++; }

    int32_t next_pseudo_plan_node_id() { return _next_pseudo_plan_node_id--; }

    size_t degree_of_parallelism() const { return _degree_of_parallelism; }

    Pipelines get_pipelines() const { return _pipelines; }

    FragmentContext* fragment_context() { return _fragment_context; }

private:
    FragmentContext* _fragment_context;
    Pipelines _pipelines;
    uint32_t _next_pipeline_id = 0;
    uint32_t _next_operator_id = 0;
    int32_t _next_pseudo_plan_node_id = Operator::s_pseudo_plan_node_id_upper_bound;
    size_t _degree_of_parallelism = 1;
};

class PipelineBuilder {
public:
    PipelineBuilder(PipelineBuilderContext& context) : _context(context) {}

    // Build pipeline from exec node tree
    Pipelines build(const FragmentContext& fragment, ExecNode* exec_node);

private:
    PipelineBuilderContext& _context;
};
} // namespace pipeline
} // namespace starrocks
