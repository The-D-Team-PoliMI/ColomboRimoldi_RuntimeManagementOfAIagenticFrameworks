#include <iostream>
#include <memory>
#include <agents-cpp/context.h>
#include <agents-cpp/llm_interface.h>
#include "lookup_sales_data.h"
#include "analyzing_data.h"

using namespace agent_cpp;

int main() {

    std::string model = "mistral-8k";
    
    JsonObject state = {
        {"prompt", "Return the top 5 stores by total revenue, identified by store number."},
        {"visualization_goal", ""},
    };

    auto llm = agents::createLLM("ollama", "", model);
    llm->setApiBase("http://127.0.0.1:11434/api");

    auto options = llm->getOptions();
    options.timeout_ms = 600000;
    llm->setOptions(options);

    auto context = std::make_shared<agents::Context>();
    context->setLLM(llm);

    auto schema = agent_cpp::DatabaseSchema::from_data_dir("../data");

    LookupSalesData lookup_agent(context, schema);

    if (!lookup_agent.init_database()){
        std::cerr << "Errore nel caricamento del database Parquet." << std::endl;
        return 1;
    }

    AnalyzingData analysis_agent(context);

    // 1. Step: Lookup SQL su DuckDB
    std::cout << "Initizalizing lookup sales agent\n\n";
    std::cout << "Model: " << model << std::endl;
    std::cout << "Prompt: " << state.value("prompt", "") << std::endl;

    state = lookup_agent.run(state);

    // 2. Step: Analisi testuale dei dati ottenuti
    std::cout << "Initizalizing analyzing data agent\n\n";
    state = analysis_agent.run(state);

    if (state.contains("error") && !state["error"].get<std::string>().empty()) 
        std::cout << "[Pipeline Error] " << state["error"].get<std::string>() << "\n";

    return 0;
}