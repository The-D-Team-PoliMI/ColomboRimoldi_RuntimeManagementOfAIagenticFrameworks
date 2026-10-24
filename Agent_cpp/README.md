
## Workflow:
   * **`decide_tool`**: Nodo router che valuta lo stato della conversazione e sceglie dinamicamente il prossimo strumento da invocare.
   * **`lookup_sales_data`**: Traduce la domanda in linguaggio naturale in una query SQL, la esegue sui file Parquet ed esporta il risultato in formato CSV.
   * **`analyzing_data`**: Genera un'analisi testuale accurata dei dati estratti.
   * **`create_visualization`**: Crea la visualizzazione grafica richiesta.

 **Chain-of-Thought**:
   
   Per migliorare l'output ogni nodo supporta un numero configurabile di iterazioni CoT: l'output prodotto dall'LLM viene raffinato iterativamente, anche gli eventuali errori prodotti dalla query o le eccezioni lanciate da matplotlib vengono iniettate nel prompt per forzare l'LLM a correggere il codice.
   
---

## Struttura

```text
Agent_cpp/
├── CMakeLists.txt         
├── Makefile                
├── include/
│   ├── config.h            # Configurazione globale (provider, modello, API key) e per-step 
│   ├── data_agent.h        # Classe SalesDataAgent: orchestratore del grafo e motore CoT
│   ├── schema.h            # Strutture dati per il caricamento degli schemi YAML delle tabelle
│   ├── steps.h             # Dichiarazioni dei 4 nodi dell'agente e formattazione CoT
│   └── utils.h             
├── src/
│   ├── main.cpp            # Entry-point
│   ├── data_agent.cpp      
│   ├── schema.cpp         
│   ├── steps.cpp           
│   └── utils.cpp           
└── third_party/
    └── json/              
```

---

## Prerequisiti

Per compilare ed eseguire correttamente il progetto in ambiente Linux sono necessari:

* **Compilatore C++20** 
* **CMake**  **Make**
* **yaml-cpp** (libreria di sistema per il parsing degli schemi database)
* **Python 3** con `matplotlib`, `pandas` e `numpy` 

Per installare le dipendenze:
```bash
sudo apt update
sudo apt install build-essential cmake libyaml-cpp-dev python3 python3-matplotlib python3-pandas python3-numpy
```
* **agents-sdk (agents.cpp)** (posizionato in `../../agents-sdk` rispetto alla cartella `Agent_cpp`)

---

### Compilazione ed Esecuzione


```bash
make
```
*(La prima esecuzione impiegherà qualche secondo in più per scaricare ed estrarre automaticamente i binari di DuckDB).*



