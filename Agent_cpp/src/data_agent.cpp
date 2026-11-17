#include "data_agent.h"
#include "steps.h"
#include "utils.h"
#include <set>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <limits>
#include <filesystem>
#include <fstream>

namespace agent_cpp {

static constexpr double COT_SIMILARITY_THRESHOLD = 0.95;

SalesDataAgent::SalesDataAgent(
    std::shared_ptr<agents::Context> context,
    const AgentConfig& config,
    const DatabaseSchema& schema,
    duckdb_connection con
) : context_(context), config_(config), schema_(schema), con_(con) {}

void SalesDataAgent::apply_candidate_options(
    const StepConfig& sc,
    double temp,
    double top_p,
    std::optional<int> top_k
) {
    auto llm = context_->getLLM();
    auto opts = llm->getOptions();
    opts.temperature = temp;
    opts.top_p = top_p;
    opts.max_tokens = sc.max_tokens;

    std::string prov = sc.provider.value_or(config_.provider);
    std::transform(prov.begin(), prov.end(), prov.begin(), ::tolower);

    llm->setOptions(opts);
}

JsonObject SalesDataAgent::invoke_step_raw(const std::string& step_name, const JsonObject& state) {
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

JsonObject SalesDataAgent::finalize_cot_diagnostics(
    JsonObject result,
    const JsonObject& base_state, 
    const std::string& step_name,
    const JsonObject& diagnostics
) const {
    result.erase("_cot_previous_response");
    result.erase("_cot_execution_error");

    JsonObject per_step = base_state.value("_cot_diagnostics_per_step", JsonObject::object());
    if (result.contains("_cot_diagnostics_per_step") && result["_cot_diagnostics_per_step"].is_object()) {
        for (auto& [k, v] : result["_cot_diagnostics_per_step"].items()) {
            per_step[k] = v;
        }
    }
    per_step[step_name] = diagnostics;
    result["_cot_diagnostics_per_step"] = per_step;
    return result;
}

JsonObject SalesDataAgent::apply_cot_iterations(
    const std::string& step_name,
    const JsonObject& base_state, 
    const JsonObject& initial_result,
    int cot_n
) {
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

    if (requested_iterations <= 1)
        return finalize_cot_diagnostics(initial_result, base_state, step_name, diagnostics);

    JsonObject result = initial_result;
    std::string previous_output = extract_step_output(step_name, result);
    std::string execution_error = result.value("error", "");

    for (int cot_i = 1; cot_i < requested_iterations; ++cot_i) {
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

        if (ratio >= COT_SIMILARITY_THRESHOLD) {
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

JsonObject SalesDataAgent::execute_step_with_config(const std::string& step_name, const JsonObject& state) {
    StepConfig sc = config_.get_step_config(step_name);
    if (!sc.enabled) {
        std::cout << "[" << step_name << "] Step disabled, skipping\n";
        return state;
    }

    int n = std::max(1, sc.n);
    auto candidate_params = sc.get_candidate_params();
    const std::string& bon_param = sc.bon_param;
    bool has_evaluator = (sc.eval_mode != "none") && (step_name != "decide_tool");

    auto run_candidate = [&](int idx, double temp, double top_p, std::optional<int> top_k) -> JsonObject {
        apply_candidate_options(sc, temp, top_p, top_k);

        if (sc.cot_n > 1) {
            std::cout << "[" << step_name << "] CoT iteration 1/" << sc.cot_n
                      << ": starting initial run...\n";
        }

        JsonObject initial_res = invoke_step_raw(step_name, state);

        if (sc.cot_n > 1) {
            initial_res = apply_cot_iterations(step_name, state, initial_res, sc.cot_n);
        }

        initial_res["_temperature"] = temp;
        initial_res["_top_p"] = top_p;
        if (top_k.has_value()) initial_res["_top_k"] = *top_k;
        initial_res["_bon_param"] = bon_param;
        initial_res["_run_idx"] = idx;
        return initial_res;
    };

    // Fast path per n == 1 (equivalente a `if n == 1:` nel codice Python originale)
    if (n == 1) {
        const auto& [temp, top_p, top_k] = candidate_params[0];
        try {
            return run_candidate(0, temp, top_p, top_k);
        } catch (const std::exception& e) {
            std::cerr << "[" << step_name << "] Error: " << e.what() << "\n";
            JsonObject err_res = state;
            err_res["error"] = e.what();
            return err_res;
        }
    }

    // Best-of-N per n > 1
    std::cout << "[" << step_name << "] Running best-of-" << n << " varying " << bon_param << ": [";
    for (size_t i = 0; i < candidate_params.size(); ++i) {
        const auto& [t, p, k] = candidate_params[i];
        if (bon_param == "top_p") std::cout << p;
        else if (bon_param == "top_k") std::cout << (k.has_value() ? std::to_string(*k) : "none");
        else std::cout << t;
        if (i + 1 < candidate_params.size()) std::cout << ", ";
    }
    std::cout << "]\n";

    std::vector<JsonObject> results;
    std::vector<double> scores;
    results.reserve(static_cast<size_t>(n));
    scores.reserve(static_cast<size_t>(n));

    for (int i = 0; i < n; ++i) {
        if (i > 0) {
            std::cout << "\n\n";
        }
        const auto& [temp, top_p, top_k] = candidate_params[static_cast<size_t>(i)];

        std::string varying_str = (bon_param == "top_p")
            ? std::to_string(top_p)
            : (bon_param == "top_k"
                ? (top_k.has_value() ? std::to_string(*top_k) : "none")
                : std::to_string(temp));

        try {
            JsonObject res = run_candidate(i, temp, top_p, top_k);

            // Salvataggio artifact temporaneo per candidato se applicabile
            if (step_name == "lookup_sales_data") {
                std::string cand_csv = "Output/result_cand_" + std::to_string(i) + ".csv";
                std::ofstream f(cand_csv);
                if (f.is_open()) {
                    f << res.value("data", "");
                    f.close();
                }
            } else if (step_name == "create_visualization") {
                if (std::filesystem::exists("Output/chart.png")) {
                    std::filesystem::copy_file(
                        "Output/chart.png",
                        "Output/chart_cand_" + std::to_string(i) + ".png",
                        std::filesystem::copy_options::overwrite_existing
                    );
                }
            }

            double score = 0.0;
            if (has_evaluator) {
                if (step_name == "lookup_sales_data") {
                    // Valutazione batch per consenso dopo aver raccolto tutti i candidati
                    score = 0.0;
                    std::cout << "  Run " << (i + 1) << "/" << n << " (" << bon_param << "=" << varying_str
                              << "): score=pending (batch eval)\n";
                } else if (step_name == "analyzing_data") {
                    std::string ans = "";
                    if (res.contains("answer") && res["answer"].is_array() && !res["answer"].empty()) {
                        ans = res["answer"].back().get<std::string>();
                    }
                    score = judge_analysis_text(
                        state.value("prompt", ""),
                        res.value("sql_query", state.value("sql_query", "")),
                        res.value("data", state.value("data", "")),
                        ans,
                        context_
                    );
                    std::cout << std::fixed << std::setprecision(3);
                    std::cout << "  Run " << (i + 1) << "/" << n << " (" << bon_param << "=" << varying_str
                              << "): score=" << score << "\n";
                } else if (step_name == "create_visualization") {
                    std::string chart_code = "";
                    if (res.contains("answer") && res["answer"].is_array() && !res["answer"].empty()) {
                        chart_code = res["answer"].back().get<std::string>();
                    }
                    std::string goal = state.value("visualization_goal", "");
                    if (goal.empty()) goal = state.value("prompt", "");
                    score = judge_visualization(
                        goal,
                        res.value("chart_config", JsonObject::object()),
                        chart_code,
                        res.value("data", state.value("data", "")),
                        context_
                    );
                    std::cout << std::fixed << std::setprecision(3);
                    std::cout << "  Run " << (i + 1) << "/" << n << " (" << bon_param << "=" << varying_str
                              << "): score=" << score << "\n";
                }
            } else {
                score = 0.0;
                std::cout << "  Run " << (i + 1) << "/" << n << " (" << bon_param << "=" << varying_str
                          << "): done (no evaluator set)\n";
            }

            results.push_back(std::move(res));
            scores.push_back(score);
        } catch (const std::exception& e) {
            std::cout << "  Run " << (i + 1) << "/" << n << " failed: " << e.what() << "\n";
            JsonObject err_res = state;
            err_res["error"] = e.what();
            results.push_back(std::move(err_res));
            scores.push_back(-std::numeric_limits<double>::infinity());
        }
    }

    // Valutazione batch di consenso per lookup_sales_data
    if (step_name == "lookup_sales_data" && has_evaluator) {
        std::vector<std::string> csvs;
        csvs.reserve(results.size());
        for (const auto& r : results) {
            csvs.push_back(r.value("data", ""));
        }
        scores = evaluate_csv_consensus(csvs);
        std::cout << "[lookup_sales_data] Batch eval scores: [";
        for (size_t i = 0; i < scores.size(); ++i) {
            std::cout << std::fixed << std::setprecision(3) << scores[i];
            if (i + 1 < scores.size()) std::cout << ", ";
        }
        std::cout << "]\n";
    }

    // Selezione del candidato migliore tramite argmax (primo massimo in caso di parità, come numpy.argmax)
    bool all_failed = scores.empty() || std::all_of(scores.begin(), scores.end(), [](double s) {
        return s == -std::numeric_limits<double>::infinity();
    });

    int best_idx = 0;
    if (!all_failed) {
        double max_score = scores[0];
        for (size_t i = 1; i < scores.size(); ++i) {
            if (scores[i] > max_score) {
                max_score = scores[i];
                best_idx = static_cast<int>(i);
            }
        }
    }

    JsonObject best_result = results.empty() ? state : results[static_cast<size_t>(best_idx)];
    best_result["_best_idx"] = best_idx;
    best_result["_all_scores"] = scores;
    double winning_score = (!scores.empty()) ? scores[static_cast<size_t>(best_idx)] : 0.0;
    std::cout << "[" << step_name << "] Selected run " << (best_idx + 1) << "/" << n
              << " (score=" << std::fixed << std::setprecision(3) << winning_score << ")\n";

    // Promozione degli artifact del vincitore su disco e rimozione dei file temporanei dei candidati
    if (step_name == "lookup_sales_data") {
        std::filesystem::path win_csv = "Output/result_cand_" + std::to_string(best_idx) + ".csv";
        if (std::filesystem::exists(win_csv)) {
            std::filesystem::copy_file(win_csv, "Output/result.csv", std::filesystem::copy_options::overwrite_existing);
        }
        for (int i = 0; i < n; ++i) {
            std::filesystem::remove("Output/result_cand_" + std::to_string(i) + ".csv");
        }
    } else if (step_name == "create_visualization") {
        std::filesystem::path win_chart = "Output/chart_cand_" + std::to_string(best_idx) + ".png";
        if (std::filesystem::exists(win_chart)) {
            std::filesystem::copy_file(win_chart, "Output/chart.png", std::filesystem::copy_options::overwrite_existing);
        }
        for (int i = 0; i < n; ++i) {
            std::filesystem::remove("Output/chart_cand_" + std::to_string(i) + ".png");
        }
    }

    return best_result;
}

JsonObject SalesDataAgent::run(const std::string& prompt) {
    return run_with_goal(prompt, "");
}

JsonObject SalesDataAgent::run_with_goal(const std::string& prompt, const std::string& visualization_goal) {
    return run(prompt, visualization_goal, config_.mode == "lookup_only", config_.mode == "analysis" || config_.mode == "lookup_only");
}

JsonObject SalesDataAgent::run(
    const std::string& prompt,
    const std::string& visualization_goal,
    bool lookup_only,
    bool no_vis
) {
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

    // Modalità lookup_only (fedele a Python run_core)
    if (lookup_only) {
        std::cout << "[Agent] Running only lookup_sales_data\n";
        return execute_step_with_config("lookup_sales_data", state);
    }

    // Modalità no_vis / analysis (fedele a Python run_core)
    if (no_vis) {
        std::cout << "[Agent] Running agent without visualization\n";
        std::cout << "\n\nTool selected: lookup_sales_data\n";
        state = execute_step_with_config("lookup_sales_data", state);
        std::cout << "\n\nTool selected: analyzing_data\n";
        state = execute_step_with_config("analyzing_data", state);
        if (state.contains("answer") && state["answer"].is_array() && !state["answer"].empty()) {
            std::cout << "\nAgent response: " << state["answer"].back().get<std::string>() << "\n";
        }
        return state;
    }

    // Esecuzione del grafo completo
    std::cout << "Running the graph...\n";
    std::set<std::string> executed_tools;
    const int max_iterations = 10;

    for (int iter = 0; iter < max_iterations; ++iter) {
        state = execute_step_with_config("decide_tool", state);
        std::string next_tool = state.value("tool_choice", "end");

        if (executed_tools.contains(next_tool)) {
            if (!executed_tools.contains("lookup_sales_data"))
                next_tool = "lookup_sales_data";
            else if (!executed_tools.contains("analyzing_data"))
                next_tool = "analyzing_data";
            else if (!executed_tools.contains("create_visualization") && !visualization_goal.empty())
                next_tool = "create_visualization";
            else
                next_tool = "end";
        }

        state["tool_choice"] = next_tool;

        if (next_tool == "end" || next_tool.empty()) {
            std::cout << "[SalesDataAgent] Transizione verso END. Workflow completato.\n";
            break;
        }

        std::cout << "[SalesDataAgent] Routing verso il nodo: " << next_tool << "\n";
        state = execute_step_with_config(next_tool, state);
        executed_tools.insert(next_tool);

        if (state.contains("error") && !state["error"].get<std::string>().empty()) {
            std::cerr << "[SalesDataAgent] Errore nel nodo " << next_tool << ": "
                      << state["error"].get<std::string>() << "\n";
            break;
        }
    }

    std::cout << "[Graph] Execution completed.\n";
    return state;
}

} // namespace agent_cpp
