#include "data_agent.h"
#include "utils.h"
#include <set>
#include <iostream>
#include <iomanip>
#include <algorithm>

namespace agent_cpp {

static constexpr double COT_SIMILARITY_THRESHOLD = 0.95;

SalesDataAgent::SalesDataAgent(std::shared_ptr<agents::Context> context, const AgentConfig& config, const DatabaseSchema& schema, duckdb_connection con): 
    context_(context), config_(config), schema_(schema), con_(con) {}

void SalesDataAgent::apply_step_options(const StepConfig& sc){
    auto llm = context_->getLLM();
    auto opts = llm->getOptions();
    opts.temperature = sc.temperature;
    opts.top_p = sc.top_p;
    opts.max_tokens = sc.max_tokens;

    llm->setOptions(opts);
}

JsonObject SalesDataAgent::execute_step_with_config(const std::string& step_name, JsonObject& state){
    StepConfig sc = config_.get_step_config(step_name);
    if (!sc.enabled)
        return state;

    apply_step_options(sc);

    JsonObject initial_result = invoke_step_raw(step_name, state);

    if (sc.cot_n > 1)
        return apply_cot_iterations(step_name, state, initial_result, sc.cot_n);

    return initial_result;
}

JsonObject SalesDataAgent::invoke_step_raw(const std::string& step_name, JsonObject& state) {
    if (step_name == "decide_tool")
        return DecideTool(state, context_);
    else if (step_name == "lookup_sales_data")
        return LookupSalesData(state, context_, schema_, con_);
    else if (step_name == "analyzing_data")
        return AnalyzingData(state, context_);
    else if (step_name == "create_visualization")
        return CreateVisualization(state, context_);
    
    return state;
}

JsonObject SalesDataAgent::finalize_cot_diagnostics(JsonObject result, const JsonObject& base_state, 
                                                    const std::string& step_name, const JsonObject& diagnostics) const {
    result.erase("_cot_previous_response");
    result.erase("_cot_execution_error");

    JsonObject per_step = base_state.value("_cot_diagnostics_per_step", JsonObject::object());
    if(result.contains("_cot_diagnostics_per_step") && result["_cot_diagnostics_per_step"].is_object()){
        for(auto& [k, v] : result["_cot_diagnostics_per_step"].items()){
            per_step[k] = v;
        }
    }
    per_step[step_name] = diagnostics;
    result["_cot_diagnostics_per_step"] = per_step;
    return result;
}

JsonObject SalesDataAgent::apply_cot_iterations(const std::string& step_name, const JsonObject& base_state, 
                                               const JsonObject& initial_result, int cot_n) {
    int requested_iterations = std::max(cot_n, 1);
    JsonObject diagnostics = {
        {"requested_iterations", requested_iterations},
        {"attempted_iterations", 1},
        {"executed_iterations", 1},
        {"early_stop", false},
        {"stop_reason", "requested_depth_reached"},
        {"similarity_threshold", COT_SIMILARITY_THRESHOLD},
        {"similarities", JsonObject::array()},
        {"final_similarity", nullptr}
    };

    if(requested_iterations <= 1)
        return finalize_cot_diagnostics(initial_result, base_state, step_name, diagnostics);

    JsonObject result = initial_result;
    std::string previous_output = extract_step_output(step_name, result);
    std::string execution_error = result.value("error", "");

    for (int cot_i = 1; cot_i < requested_iterations; ++cot_i){
        diagnostics["attempted_iterations"] = cot_i + 1;
        std::cout << "\n[" << step_name << "] CoT iteration " << (cot_i + 1)
                  << "/" << requested_iterations << ": starting refinement...\n";

        JsonObject iter_state = base_state;
        iter_state["_cot_previous_response"] = previous_output;
        iter_state["_cot_execution_error"] = execution_error;
        iter_state["error"] = "";

        JsonObject new_result;
        try {
            new_result = invoke_step_raw(step_name, iter_state);
        } catch (const std::exception& e) {
            diagnostics["early_stop"] = diagnostics["executed_iterations"].get<int>() < requested_iterations;
            diagnostics["stop_reason"] = "refinement_error";
            std::cout << "[" << step_name << "] CoT iteration " << (cot_i + 1)
                      << "/" << requested_iterations << " failed: " << e.what() << "\n";
            break;
        }

        diagnostics["executed_iterations"] = cot_i + 1;
        std::string new_error = new_result.value("error", "");
        if (!new_error.empty()) {
            std::cout << "[" << step_name << "] CoT iteration " << (cot_i + 1)
                      << "/" << requested_iterations << ": execution error — " << new_error << "\n";
            result = new_result;
            previous_output = extract_step_output(step_name, new_result);
            execution_error = new_error;
            continue;
        }

        std::string new_output = extract_step_output(step_name, new_result);
        double ratio = sequence_similarity(previous_output, new_output);
        std::cout << std::fixed << std::setprecision(3)
                  << "[" << step_name << "] CoT iteration " << (cot_i + 1)
                  << "/" << requested_iterations << ": similarity=" << ratio << "\n";

        result = new_result;
        execution_error = "";
        diagnostics["similarities"].push_back(ratio);
        diagnostics["final_similarity"] = ratio;

        if (ratio >= COT_SIMILARITY_THRESHOLD){
            diagnostics["early_stop"] = diagnostics["executed_iterations"].get<int>() < requested_iterations;
            diagnostics["stop_reason"] = "converged";
            std::cout << "[" << step_name << "] CoT early stop: output converged (similarity="
                      << ratio << " >= " << COT_SIMILARITY_THRESHOLD << ")\n";
            break;
        }

        previous_output = new_output;
    }

    return finalize_cot_diagnostics(result, base_state, step_name, diagnostics);
}

JsonObject SalesDataAgent::run(const std::string& prompt) {
    return run_with_goal(prompt, "");
}

JsonObject SalesDataAgent::run_with_goal(const std::string& prompt, const std::string& visualization_goal){
    JsonObject state = {
        {"prompt", prompt},
        {"visualization_goal", visualization_goal},
        {"data", ""},
        {"sql_query", ""},
        {"chart_config", JsonObject::object()},
        {"answer", JsonObject::array()},
        {"tool_choice", "None"},
        {"error", ""}
    };

    std::set<std::string> executed_tools;
    const int max_iterations = 6; // Evitiamo loop infiniti

    for (int iter = 0; iter < max_iterations; ++iter){
        state = execute_step_with_config("decide_tool", state);
        std::string next_tool = state.value("tool_choice", "end"); // restituisce il tool scelto o end se tool choice non esiste nello state o è null

        if(config_.mode == "lookup_only" && executed_tools.contains("lookup_sales_data"))
            next_tool = "end";
        else if(config_.mode == "analysis" && executed_tools.contains("analyzing_data"))
            next_tool = "end";

        // guardrail deterministico in caso di errore di DecideTool
        if(executed_tools.contains(next_tool)){
            if (!executed_tools.contains("lookup_sales_data"))
                next_tool = "lookup_sales_data";
            else if (!executed_tools.contains("analyzing_data") && config_.mode != "lookup_only")
                next_tool = "analyzing_data";
            else if (!executed_tools.contains("create_visualization") && config_.mode == "full" && !visualization_goal.empty())
                next_tool = "create_visualization";
            else
                next_tool = "end";
        }

        state["tool_choice"] = next_tool;

        if(next_tool == "end" || next_tool.empty()){
            std::cout << "[SalesDataAgent] Transizione verso END. Workflow completato.\n";
            break;
        }

        std::cout << "[SalesDataAgent] Routing verso il nodo: " << next_tool << "\n";
        state = execute_step_with_config(next_tool, state);
        executed_tools.insert(next_tool);

        if(state.contains("error") && !state["error"].get<std::string>().empty()){
            std::cerr << "[SalesDataAgent] Errore nel nodo " << next_tool << ": "
                      << state["error"].get<std::string>() << "\n";
            break;
        }
    }
    return state;
}

} 