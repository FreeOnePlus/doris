// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <gen_cpp/Metrics_types.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "common/config.h"
#include "common/status.h"
#include "udf/udf.h"
#include "util/runtime_profile.h"
#include "vec/exprs/vexpr.h"

namespace doris {
class RowDescriptor;
class RuntimeState;
class TExprNode;

double get_in_list_ignore_thredhold(size_t list_size);
double get_comparison_ignore_thredhold();
double get_bloom_filter_ignore_thredhold();
} // namespace doris

namespace doris::vectorized {
#include "common/compile_check_begin.h"

class Block;
class VExprContext;

class VRuntimeFilterWrapper final : public VExpr {
    ENABLE_FACTORY_CREATOR(VRuntimeFilterWrapper);

public:
    VRuntimeFilterWrapper(const TExprNode& node, VExprSPtr impl, double ignore_thredhold,
                          bool null_aware, int filter_id);
    ~VRuntimeFilterWrapper() override = default;
    Status execute(VExprContext* context, Block* block, int* result_column_id) override;
    Status prepare(RuntimeState* state, const RowDescriptor& desc, VExprContext* context) override;
    Status open(RuntimeState* state, VExprContext* context,
                FunctionContext::FunctionStateScope scope) override;
    std::string debug_string() const override { return _impl->debug_string(); }
    void close(VExprContext* context, FunctionContext::FunctionStateScope scope) override;
    const std::string& expr_name() const override;
    const VExprSPtrs& children() const override { return _impl->children(); }

    VExprSPtr get_impl() const override { return _impl; }

    void attach_profile_counter(std::shared_ptr<RuntimeProfile::Counter> rf_input_rows,
                                std::shared_ptr<RuntimeProfile::Counter> rf_filter_rows,
                                std::shared_ptr<RuntimeProfile::Counter> always_true_filter_rows) {
        DCHECK(rf_input_rows != nullptr);
        DCHECK(rf_filter_rows != nullptr);
        DCHECK(always_true_filter_rows != nullptr);

        if (rf_input_rows != nullptr) {
            _rf_input_rows = rf_input_rows;
        }
        if (rf_filter_rows != nullptr) {
            _rf_filter_rows = rf_filter_rows;
        }
        if (always_true_filter_rows != nullptr) {
            _always_true_filter_rows = always_true_filter_rows;
        }
    }

    void update_counters(int64_t filter_rows, int64_t input_rows) {
        COUNTER_UPDATE(_rf_filter_rows, filter_rows);
        COUNTER_UPDATE(_rf_input_rows, input_rows);
    }

    template <typename T>
    static void judge_selectivity(double ignore_threshold, int64_t filter_rows, int64_t input_rows,
                                  T& always_true) {
        always_true = static_cast<double>(filter_rows) / static_cast<double>(input_rows) <
                      ignore_threshold;
    }

    bool is_rf_wrapper() const override { return true; }

    int filter_id() const { return _filter_id; }

    void do_judge_selectivity(uint64_t filter_rows, uint64_t input_rows) override {
        update_counters(filter_rows, input_rows);

        if (!_always_true) {
            _judge_filter_rows += filter_rows;
            _judge_input_rows += input_rows;
            judge_selectivity(_ignore_thredhold, _judge_filter_rows, _judge_input_rows,
                              _always_true);
        }
    }

    std::shared_ptr<RuntimeProfile::Counter> predicate_filtered_rows_counter() const {
        return _rf_filter_rows;
    }
    std::shared_ptr<RuntimeProfile::Counter> predicate_input_rows_counter() const {
        return _rf_input_rows;
    }

private:
    void reset_judge_selectivity() {
        _always_true = false;
        _judge_counter = config::runtime_filter_sampling_frequency;
        _judge_input_rows = 0;
        _judge_filter_rows = 0;
    }

    VExprSPtr _impl;
    // VRuntimeFilterWrapper and ColumnPredicate share the same logic,
    // but it's challenging to unify them, so the code is duplicated.
    // _judge_counter, _judge_input_rows, _judge_filter_rows, and _always_true
    // are variables used to implement the _always_true logic, calculated periodically
    // based on runtime_filter_sampling_frequency. During each period, if _always_true
    // is evaluated as true, the logic for always_true is applied for the rest of that period
    // without recalculating. At the beginning of the next period,
    // reset_judge_selectivity is used to reset these variables.
    std::atomic_int _judge_counter = 0;
    std::atomic_uint64_t _judge_input_rows = 0;
    std::atomic_uint64_t _judge_filter_rows = 0;
    std::atomic_int _always_true = false;

    std::shared_ptr<RuntimeProfile::Counter> _rf_input_rows =
            std::make_shared<RuntimeProfile::Counter>(TUnit::UNIT, 0);
    std::shared_ptr<RuntimeProfile::Counter> _rf_filter_rows =
            std::make_shared<RuntimeProfile::Counter>(TUnit::UNIT, 0);
    std::shared_ptr<RuntimeProfile::Counter> _always_true_filter_rows =
            std::make_shared<RuntimeProfile::Counter>(TUnit::UNIT, 0);

    std::string _expr_name;
    double _ignore_thredhold;
    bool _null_aware;
    int _filter_id;
};

using VRuntimeFilterPtr = std::shared_ptr<VRuntimeFilterWrapper>;

#include "common/compile_check_end.h"
} // namespace doris::vectorized