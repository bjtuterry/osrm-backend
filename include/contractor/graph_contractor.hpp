#ifndef OSRM_CONTRACTOR_GRAPH_CONTRACTOR_HPP
#define OSRM_CONTRACTOR_GRAPH_CONTRACTOR_HPP

#include "contractor/contractor_dijkstra.hpp"
#include "contractor/contractor_graph.hpp"
#include "contractor/query_edge.hpp"
#include "util/deallocating_vector.hpp"
#include "util/integer_range.hpp"
#include "util/log.hpp"
#include "util/percent.hpp"
#include "util/timing_util.hpp"
#include "util/typedefs.hpp"
#include "util/xor_fast_hash.hpp"

#include <boost/assert.hpp>

#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_sort.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <vector>

#if USE_STXXL_LIBRARY
#include <stxxl/vector>
#endif

namespace osrm
{
namespace contractor
{

class GraphContractor
{
  private:
#if USE_STXXL_LIBRARY
    template <typename T> using ExternalVector = stxxl::vector<T>;
#else
    template <typename T> using ExternalVector = std::vector<T>;
#endif

    struct ContractorThreadData
    {
        ContractorDijkstra dijkstra;
        std::vector<ContractorEdge> inserted_edges;
        std::vector<NodeID> neighbours;
        explicit ContractorThreadData(NodeID nodes) : dijkstra(nodes) {}
    };

    using NodeDepth = int;

    struct ContractionStats
    {
        int edges_deleted_count;
        int edges_added_count;
        int original_edges_deleted_count;
        int original_edges_added_count;
        ContractionStats()
            : edges_deleted_count(0), edges_added_count(0), original_edges_deleted_count(0),
              original_edges_added_count(0)
        {
        }
    };

    struct RemainingNodeData
    {
        RemainingNodeData() : id(0), is_independent(false) {}
        NodeID id : 31;
        bool is_independent : 1;
    };

    struct ThreadDataContainer
    {
        explicit ThreadDataContainer(int number_of_nodes) : number_of_nodes(number_of_nodes) {}

        inline ContractorThreadData *GetThreadData()
        {
            bool exists = false;
            auto &ref = data.local(exists);
            if (!exists)
            {
                // ref = std::make_shared<ContractorThreadData>(number_of_nodes);
                ref = std::make_shared<ContractorThreadData>(4000);
            }

            return ref.get();
        }

        int number_of_nodes;
        using EnumerableThreadData =
            tbb::enumerable_thread_specific<std::shared_ptr<ContractorThreadData>>;
        EnumerableThreadData data;
    };

  public:
    GraphContractor(ContractorGraph &graph);

    GraphContractor(ContractorGraph &graph,
                    std::vector<float> node_levels_,
                    std::vector<EdgeWeight> node_weights_);

    void Run(double core_factor = 1.0);

    std::vector<bool> GetCoreMarker();

    std::vector<float> GetNodeLevels();


  private:
    void RenumberGraph(ThreadDataContainer &thread_data_list,
                       std::vector<RemainingNodeData> &remaining_nodes,
                       std::vector<float> &node_priorities);

    float EvaluateNodePriority(ContractorThreadData *const data,
                               const NodeDepth node_depth,
                               const NodeID node);

    template <bool RUNSIMULATION>
    bool
    ContractNode(ContractorThreadData *data, const NodeID node, ContractionStats *stats = nullptr)
    {
        auto &dijkstra = data->dijkstra;
        std::size_t inserted_edges_size = data->inserted_edges.size();
        std::vector<ContractorEdge> &inserted_edges = data->inserted_edges;
        constexpr bool SHORTCUT_ARC = true;
        constexpr bool FORWARD_DIRECTION_ENABLED = true;
        constexpr bool FORWARD_DIRECTION_DISABLED = false;
        constexpr bool REVERSE_DIRECTION_ENABLED = true;
        constexpr bool REVERSE_DIRECTION_DISABLED = false;

        for (auto in_edge : graph.GetAdjacentEdgeRange(node))
        {
            const ContractorEdgeData &in_data = graph.GetEdgeData(in_edge);
            const NodeID source = graph.GetTarget(in_edge);
            if (source == node)
                continue;

            if (RUNSIMULATION)
            {
                BOOST_ASSERT(stats != nullptr);
                ++stats->edges_deleted_count;
                stats->original_edges_deleted_count += in_data.originalEdges;
            }
            if (!in_data.backward)
            {
                continue;
            }

            dijkstra.Clear();
            dijkstra.Insert(source, 0, ContractorHeapData{});
            EdgeWeight max_weight = 0;
            unsigned number_of_targets = 0;

            for (auto out_edge : graph.GetAdjacentEdgeRange(node))
            {
                const ContractorEdgeData &out_data = graph.GetEdgeData(out_edge);
                if (!out_data.forward)
                {
                    continue;
                }
                const NodeID target = graph.GetTarget(out_edge);
                if (node == target)
                {
                    continue;
                }

                const EdgeWeight path_weight = in_data.weight + out_data.weight;
                if (target == source)
                {
                    if (path_weight < node_weights[node])
                    {
                        if (RUNSIMULATION)
                        {
                            // make sure to prune better, but keep inserting this loop if it should
                            // still be the best
                            // CAREFUL: This only works due to the independent node-setting. This
                            // guarantees that source is not connected to another node that is
                            // contracted
                            node_weights[source] = path_weight + 1;
                            BOOST_ASSERT(stats != nullptr);
                            stats->edges_added_count += 2;
                            stats->original_edges_added_count +=
                                2 * (out_data.originalEdges + in_data.originalEdges);
                        }
                        else
                        {
                            // CAREFUL: This only works due to the independent node-setting. This
                            // guarantees that source is not connected to another node that is
                            // contracted
                            node_weights[source] = path_weight; // make sure to prune better
                            inserted_edges.emplace_back(source,
                                                        target,
                                                        path_weight,
                                                        in_data.duration + out_data.duration,
                                                        out_data.originalEdges +
                                                            in_data.originalEdges,
                                                        node,
                                                        SHORTCUT_ARC,
                                                        FORWARD_DIRECTION_ENABLED,
                                                        REVERSE_DIRECTION_DISABLED);

                            inserted_edges.emplace_back(target,
                                                        source,
                                                        path_weight,
                                                        in_data.duration + out_data.duration,
                                                        out_data.originalEdges +
                                                            in_data.originalEdges,
                                                        node,
                                                        SHORTCUT_ARC,
                                                        FORWARD_DIRECTION_DISABLED,
                                                        REVERSE_DIRECTION_ENABLED);
                        }
                    }
                    continue;
                }
                max_weight = std::max(max_weight, path_weight);
                if (!dijkstra.WasInserted(target))
                {
                    dijkstra.Insert(target, INVALID_EDGE_WEIGHT, ContractorHeapData{0, true});
                    ++number_of_targets;
                }
            }

            if (RUNSIMULATION)
            {
                const int constexpr SIMULATION_SEARCH_SPACE_SIZE = 1000;
                dijkstra.Run(
                    number_of_targets, SIMULATION_SEARCH_SPACE_SIZE, max_weight, node, graph);
            }
            else
            {
                const int constexpr FULL_SEARCH_SPACE_SIZE = 2000;
                dijkstra.Run(number_of_targets, FULL_SEARCH_SPACE_SIZE, max_weight, node, graph);
            }
            for (auto out_edge : graph.GetAdjacentEdgeRange(node))
            {
                const ContractorEdgeData &out_data = graph.GetEdgeData(out_edge);
                if (!out_data.forward)
                {
                    continue;
                }
                const NodeID target = graph.GetTarget(out_edge);
                if (target == node)
                    continue;

                const EdgeWeight path_weight = in_data.weight + out_data.weight;
                const EdgeWeight weight = dijkstra.GetKey(target);
                if (path_weight < weight)
                {
                    if (RUNSIMULATION)
                    {
                        BOOST_ASSERT(stats != nullptr);
                        stats->edges_added_count += 2;
                        stats->original_edges_added_count +=
                            2 * (out_data.originalEdges + in_data.originalEdges);
                    }
                    else
                    {
                        inserted_edges.emplace_back(source,
                                                    target,
                                                    path_weight,
                                                    in_data.duration + out_data.duration,
                                                    out_data.originalEdges + in_data.originalEdges,
                                                    node,
                                                    SHORTCUT_ARC,
                                                    FORWARD_DIRECTION_ENABLED,
                                                    REVERSE_DIRECTION_DISABLED);

                        inserted_edges.emplace_back(target,
                                                    source,
                                                    path_weight,
                                                    in_data.duration + out_data.duration,
                                                    out_data.originalEdges + in_data.originalEdges,
                                                    node,
                                                    SHORTCUT_ARC,
                                                    FORWARD_DIRECTION_DISABLED,
                                                    REVERSE_DIRECTION_ENABLED);
                    }
                }
            }
        }
        // Check For One-Way Streets to decide on the creation of self-loops

        if (!RUNSIMULATION)
        {
            std::size_t iend = inserted_edges.size();
            for (std::size_t i = inserted_edges_size; i < iend; ++i)
            {
                bool found = false;
                for (std::size_t other = i + 1; other < iend; ++other)
                {
                    if (inserted_edges[other].source != inserted_edges[i].source)
                    {
                        continue;
                    }
                    if (inserted_edges[other].target != inserted_edges[i].target)
                    {
                        continue;
                    }
                    if (inserted_edges[other].data.weight != inserted_edges[i].data.weight)
                    {
                        continue;
                    }
                    if (inserted_edges[other].data.shortcut != inserted_edges[i].data.shortcut)
                    {
                        continue;
                    }
                    inserted_edges[other].data.forward |= inserted_edges[i].data.forward;
                    inserted_edges[other].data.backward |= inserted_edges[i].data.backward;
                    found = true;
                    break;
                }
                if (!found)
                {
                    inserted_edges[inserted_edges_size++] = inserted_edges[i];
                }
            }
            inserted_edges.resize(inserted_edges_size);
        }
        return true;
    }

    void DeleteIncomingEdges(ContractorThreadData *data, const NodeID node);

    bool UpdateNodeNeighbours(std::vector<float> &priorities,
                              std::vector<NodeDepth> &node_depth,
                              ContractorThreadData *const data,
                              const NodeID node);

    bool IsNodeIndependent(const std::vector<float> &priorities,
                           ContractorThreadData *const data,
                           NodeID node) const;

    // This bias function takes up 22 assembly instructions in total on X86
    bool Bias(const NodeID a, const NodeID b) const;

    ContractorGraph &graph;
    std::vector<NodeID> orig_node_id_from_new_node_id_map;
    std::vector<float> node_levels;

    // A list of weights for every node in the graph.
    // The weight represents the cost for a u-turn on the segment in the base-graph in addition to
    // its traversal.
    // During contraction, self-loops are checked against this node weight to ensure that necessary
    // self-loops are added.
    std::vector<EdgeWeight> node_weights;
    std::vector<bool> is_core_node;
    util::XORFastHash<> fast_hash;
};

} // namespace contractor
} // namespace osrm

#endif // OSRM_CONTRACTOR_GRAPH_CONTRACTOR_HPP
