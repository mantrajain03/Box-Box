#ifndef BOXBOX_CLI_ALGORITHM_CATALOG_HPP
#define BOXBOX_CLI_ALGORITHM_CATALOG_HPP

#include <string>
#include <vector>

namespace boxbox {

/// What an operation costs, as the code's own comments and docs/COMPLEXITY_AUDIT.md state it.
struct AlgorithmCost {
    std::string operation;
    std::string cost;
};

/// One algorithm or data structure the project implements, for the frontend's Algorithms
/// page. The costs are copied from the headers and the complexity audit, not derived here;
/// each `files` entry is a path from the repository root, and a test checks every one exists,
/// so a card on the page can always be traced to code.
struct AlgorithmInfo {
    std::string id;
    std::string name;
    std::string category;                ///< "Optimisation", "Heaps", "Ranges", "Windows", "Search" or "Rules"
    std::string purpose;                 ///< what it does in this project
    std::vector<std::string> files;      ///< source files, from the repository root
    std::vector<AlgorithmCost> costs;
    std::string note;                    ///< what is easy to get wrong about the cost or the rule
    std::vector<std::string> usedIn;     ///< frontend page ids that visualise it
};

/// The whole catalog, in the order the page shows it.
const std::vector<AlgorithmInfo>& algorithmCatalog();

}  // namespace boxbox

#endif  // BOXBOX_CLI_ALGORITHM_CATALOG_HPP
