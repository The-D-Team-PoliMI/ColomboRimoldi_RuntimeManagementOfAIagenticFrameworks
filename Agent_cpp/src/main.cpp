#include <iostream>
#include <memory>
#include <string>
#include <cstdlib>
#include <agents-cpp/context.h>
#include <agents-cpp/llm_interface.h>
#include "config.h"
#include "schema.h"
#include "utils.h"
#include "data_agent.h"

using namespace agent_cpp;

int main(int argc, char** argv) {

    std::string prompt = "Show store cities where total revenue in 2022 exceeded 200,000 as a bar chart";
    std::string visualization_goal = "bar chart of store cities with total 2022 revenue above $200,000, ordered from highest to lowest";

    RunOptions run_opts;
    run_opts.save_results = false;  // true per salvare la run in cache

    // Argomenti da riga di comando (usati dal benchmark run_cpp_benchmark.py)
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--prompt" && i + 1 < argc)
            prompt = argv[++i];
        else if (arg == "--goal" && i + 1 < argc)
            visualization_goal = argv[++i];      // "" = nessun grafico
        else if (arg == "--run-id" && i + 1 < argc)
            run_opts.run_id = argv[++i];
        else if (arg == "--save")
            run_opts.save_results = true;
        else {
            std::cerr << "Argomento sconosciuto: " << arg << "\n"
                      << "Uso: " << argv[0]
                      << " [--prompt <testo>] [--goal <testo>] [--run-id <id>] [--save]\n";
            return 1;
        }
    }

    AgentConfig config;

    // Recupero dell'API key corretta in base al provider
    std::string api_key = "";
    if (config.provider == "google") {
        if (!config.gemini_api_key.empty())
            api_key = config.gemini_api_key;
        else
            std::cerr << "Attenzione: GEMINI_API_KEY non trovata in config o variabile d'ambiente.\n";
    }

    auto llm = agents::createLLM(config.provider, api_key, config.model);

    if (config.provider == "ollama") {
        llm->setApiBase(config.ollama_url);
    }

    auto options = llm->getOptions();
    options.timeout_ms = 600000;
    llm->setOptions(options);

    auto context = std::make_shared<agents::Context>();
    context->setLLM(llm);

    // Caricamento schema database
    DatabaseSchema schema;
    try {
        schema = DatabaseSchema::from_data_dir(config.data_dir);
    } catch (const std::exception& e) {
        std::cerr << "Errore caricamento schema: " << e.what() << std::endl;
        return 1;
    }
    // Inizializzazione DuckDB
    duckdb_database db = nullptr;
    duckdb_connection con = nullptr;
    if (duckdb_open(nullptr, &db) == DuckDBError || duckdb_connect(db, &con) == DuckDBError || !init_database(con, schema)) {
        std::cerr << "Errore inizializzazione DuckDB." << std::endl;
        return 1;
    }

    // Avvio del workflow
    SalesDataAgent agent(context, config, schema, con);

    std::cout << "Prompt: " << prompt << "\n";
    std::cout << "Visualization goal: " << (visualization_goal.empty() ? "(nessun grafico)" : visualization_goal) << "\n";

    JsonObject result = agent.run_with_goal(prompt, visualization_goal, run_opts);

    if (result.contains("error") && !result["error"].get<std::string>().empty()) {
        std::cout << "\n[Pipeline Error]: " << result["error"].get<std::string>() << "\n";
    }
    if (result.contains("run_id"))
        std::cout << "Run ID: " << result["run_id"].get<std::string>() << "\n";

    // Tempi della run (totale, per step e solo chiamate LLM)
    if (result.contains("_total_run_time_sec")) {
        const JsonObject step_times = result.value("_step_timings_sec", JsonObject::object());
        const JsonObject llm_times  = result.value("_step_llm_timings_sec", JsonObject::object());
        std::cout << "\nTempo totale: " << result["_total_run_time_sec"].get<double>() << " s\n";
        for (const auto& [step, sec] : step_times.items())
            std::cout << "  " << step << ": " << sec.get<double>() << " s (LLM: "
                      << llm_times.value(step, 0.0) << " s)\n";
    }

    duckdb_disconnect(&con);
    duckdb_close(&db);

    return 0;
}