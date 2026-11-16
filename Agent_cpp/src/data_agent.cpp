#include "data_agent.h"
#include "utils.h"
#include <random>
#include <set>
#include <sstream>
#include <vector>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include "timing.h"

namespace agent_cpp {

static constexpr double COT_SIMILARITY_THRESHOLD = 0.95;

SalesDataAgent::SalesDataAgent(std::shared_ptr<agents::Context> context, const AgentConfig& config, const DatabaseSchema& schema, duckdb_connection con): 
    context_(context), config_(config), schema_(schema), con_(con), cache_(config.cache_dir) {}

static bool has_error(const JsonObject& r){
    return r.contains("error") && r.at("error").is_string() && !r.at("error").get<std::string>().empty();
}

// Genera 8 caratteri esadecimali casuali
static std::string generate_run_id(){
    std::random_device rd;
    std::ostringstream ss;
    ss << std::hex << std::setw(8) << std::setfill('0') << rd();
    return ss.str();
}

// Scrive in result[key][step_name] il tempo dello step, conservando le voci degli step precedenti (prese da `state`).
// Equivale a `existing_timings[step_name] = round(elapsed, 3)` di Python.
static void record_step_time(JsonObject& result, const JsonObject& state, const char* key,
                             const std::string& step_name, double seconds){
    JsonObject times = state.value(key, JsonObject::object());
    times[step_name] = round3(seconds);
    result[key] = times;
}

void SalesDataAgent::apply_step_options(const StepConfig& sc){
    auto llm = context_->getLLM();
    auto opts = llm->getOptions();
    opts.temperature = sc.temperature;
    opts.top_p = sc.top_p;
    opts.max_tokens = sc.max_tokens;

    llm->setOptions(opts);
}

std::optional<JsonObject> SalesDataAgent::lookup_cached_step(const std::string& step_name, const JsonObject& state, const StepConfig& sc){
    if (cached_step_results_.empty())
        return std::nullopt;   

    auto it = cached_step_results_.find(step_name);
    if (it == cached_step_results_.end() || it->second.empty()){
        std::cout << "[" << step_name << "] Cache miss\n";
        return std::nullopt;
    }

    const std::vector<JsonObject>& candidates = it->second;
    std::cout << "[" << step_name << "] Found " << candidates.size() << " cached result(s)\n";

    // Selezione del candidato. Senza BoN né evaluator, Auto e Skip coincidono: si usa il primo.
    // TODO(BoN): in modalità Auto con più candidati qui andrà la ri-selezione (eval_fn + selection_fn).
    const size_t best_idx = 0;
    JsonObject result = candidates[best_idx];

    // Non si riusa uno step finito in errore: rieseguirlo è l'unico modo per ripararlo.
    if (has_error(result)){
        std::cout << "[" << step_name << "] Cached result contains an error, ignoring it\n";
        return std::nullopt;
    }

    std::cout << "[" << step_name << "] Using cached result (" << sc.cache_mode << " mode)\n";

    // Lo stato in cache è quello della run originale: identità e obiettivo restano quelli della run corrente
    // (importante quando la run è stata trovata per similarità e non è identica).
    result["prompt"] = state.value("prompt", "");
    result["visualization_goal"] = state.value("visualization_goal", "");

     // Tempi: lo stato in cache porta con sé quelli della run originale, qui restano quelli di questa run.
    // Lo step è stato riusato e non eseguito, quindi il suo tempo è zero.
    record_step_time(result, state, "_step_timings_sec", step_name, 0.0);
    record_step_time(result, state, "_step_llm_timings_sec", step_name, 0.0);

    // I dati vengono dalla cache: LookupSalesData non gira, quindi Output/result.csv va riscritto
    if (result.contains("data") && result["data"].is_string() && !result["data"].get<std::string>().empty())
        save_csv_artifact(result["data"].get<std::string>());

    // Il risultato riusato fa parte della run corrente: conservo tutti i candidati originali
    // così la run salvata è completa.
    current_run_step_results_[step_name] = candidates;
    return result;
}

void SalesDataAgent::invalidate_downstream_cache(const std::string& step_name){
    static const std::vector<std::string> pipeline = {"lookup_sales_data", "analyzing_data", "create_visualization"};
    auto it = std::find(pipeline.begin(), pipeline.end(), step_name);
    if (it == pipeline.end())
        return;
    for (++it; it != pipeline.end(); ++it){ // scarta dalla cache gli step che vengono dopo quello appena rigenerato
        if (cached_step_results_.erase(*it) > 0)
            std::cout << "[" << step_name << "] Step regenerated: discarding cached '" << *it << "'\n";
    }
}

JsonObject SalesDataAgent::execute_step_with_config(const std::string& step_name, const JsonObject& state){
    StepConfig sc = config_.get_step_config(step_name);
    if (!sc.enabled)
        return state;

    if (sc.use_cache){
        if (auto cached = lookup_cached_step(step_name, state, sc)) // se lo step ha un risultato in cache lo restituisce
            return *cached;
    }
    // Da qui lo step viene eseguito davvero: partono i cronometri (come _step_t0 in Python, dopo il blocco cache).
    const auto step_start = Clock::now();
    llm_time_accumulator() = 0.0;

    apply_step_options(sc);

    if (sc.cot_n > 1) 
        std::cout << "[" << step_name << "] CoT iteration 1/" << sc.cot_n
                  << ": starting initial run...\n";

    JsonObject result = invoke_step_raw(step_name, state);

    if (sc.cot_n > 1)
        result = apply_cot_iterations(step_name, state, result, sc.cot_n);

    // Tempo dello step (CoT incluso) e tempo delle sole chiamate LLM. Va scritto prima di salvare il risultato
    // in cache, così anche il file dello step contiene i propri tempi.
    record_step_time(result, state, "_step_timings_sec", step_name, elapsed_sec(step_start));
    record_step_time(result, state, "_step_llm_timings_sec", step_name, llm_time_accumulator());

    // Risultato (dopo l'eventuale CoT) da salvare in cache; con il BoN qui andranno tutti gli N candidati.
    current_run_step_results_[step_name].assign(1, result);
    invalidate_downstream_cache(step_name); // abbiamo prodotto un nuovo output: eliminiamo i vecchi output degli step a valle se ci sono
    return result;
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

StepResults SalesDataAgent::load_cached_results(const std::string& prompt, const RunOptions& opts){
    if (!opts.reuse_from.empty()){
        auto loaded = cache_.load_all_step_results(opts.reuse_from);
        if (!loaded.empty())
            std::cout << "[SalesDataAgent] Loaded cached results from run: " << opts.reuse_from << "\n";
        else
            std::cerr << "[SalesDataAgent] Warning: no cached results found for run: " << opts.reuse_from << "\n";
        return loaded;
    }

    if (opts.save_results){
        auto similar = cache_.find_similar_runs(prompt, 3);
        if (!similar.empty()){
            std::cout << "[SalesDataAgent] Found " << similar.size() << " similar run(s), reusing: " << similar.front() << "\n";
            return cache_.load_all_step_results(similar.front());
        }
    }
    return {};
}

void SalesDataAgent::save_run_to_cache(const std::string& run_id, const std::string& prompt,
                                       const std::string& visualization_goal, const JsonObject& final_state){
    try{
        cache_.save_run(run_id, prompt, config_.to_json(), current_run_step_results_, final_state,
                        JsonObject{{"visualization_goal", visualization_goal}, {"mode", config_.mode}});
        std::cout << "[SalesDataAgent] Run saved with ID: " << run_id << "\n";
    } catch (const std::exception& e){
        std::cerr << "[SalesDataAgent] Warning: failed to save run to cache: " << e.what() << "\n";
    }
}

JsonObject SalesDataAgent::run(const std::string& prompt, const RunOptions& opts) {
    return run_with_goal(prompt, "", opts);
}

JsonObject SalesDataAgent::run_with_goal(const std::string& prompt, const std::string& visualization_goal, const RunOptions& opts){
    const std::string run_id = opts.run_id.empty() ? generate_run_id() : opts.run_id; // se non run_id non è specificato ne genera uno casuale
    current_run_step_results_.clear();
    cached_step_results_ = load_cached_results(prompt, opts);

    const auto run_start = Clock::now();   // il caricamento della cache è escluso dal tempo totale, come in Python

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

    // Prima del salvataggio, così il tempo totale finisce anche in metadata.json.
    state["_total_run_time_sec"] = round3(elapsed_sec(run_start));

    if (opts.save_results)
        save_run_to_cache(run_id, prompt, visualization_goal, state);
    state["run_id"] = run_id;
    return state;
}

} 