// Copyright (C) 2006-2020 Anders Logg and Garth N. Wells
//
// This file is part of DOLFINX (https://www.fenicsproject.org)
//
// SPDX-License-Identifier:    LGPL-3.0-or-later

#include "utils.h"
#include "Geometry.h"
#include "MeshTags.h"
#include "cell_types.h"
#include "graphbuild.h"
#include <algorithm>
#include <cfloat>
#include <cstdlib>
#include <dolfinx/common/IndexMap.h>
#include <dolfinx/common/log.h>
#include <dolfinx/fem/ElementDofLayout.h>
#include <dolfinx/graph/partition.h>
#include <stdexcept>
#include <unordered_set>
#include <xtensor-blas/xlinalg.hpp>
#include <xtensor/xadapt.hpp>
#include <xtensor/xbuilder.hpp>
#include <xtensor/xfixed.hpp>
#include <xtensor/xmath.hpp>
#include <xtensor/xnorm.hpp>
#include <xtensor/xview.hpp>

using namespace dolfinx;

//-----------------------------------------------------------------------------
graph::AdjacencyList<std::int64_t>
mesh::extract_topology(const CellType& cell_type,
                       const fem::ElementDofLayout& layout,
                       const graph::AdjacencyList<std::int64_t>& cells)
{
  // Use ElementDofLayout to get vertex dof indices (local to a cell)
  const int num_vertices_per_cell = num_cell_vertices(cell_type);
  std::vector<int> local_vertices(num_vertices_per_cell);
  for (int i = 0; i < num_vertices_per_cell; ++i)
  {
    const std::vector<int> local_index = layout.entity_dofs(0, i);
    assert(local_index.size() == 1);
    local_vertices[i] = local_index[0];
  }

  // Extract vertices
  std::vector<std::int64_t> topology(cells.num_nodes() * num_vertices_per_cell);
  for (int c = 0; c < cells.num_nodes(); ++c)
  {
    auto p = cells.links(c);
    for (int j = 0; j < num_vertices_per_cell; ++j)
      topology[num_vertices_per_cell * c + j] = p[local_vertices[j]];
  }

  return graph::build_adjacency_list<std::int64_t>(std::move(topology),
                                                   num_vertices_per_cell);
}
//-----------------------------------------------------------------------------
std::vector<double> mesh::h(const Mesh& mesh,
                            const xtl::span<const std::int32_t>& entities,
                            int dim)
{
  if (dim != mesh.topology().dim())
    throw std::runtime_error("Cell size when dim ne tdim  requires updating.");

  // Get number of cell vertices
  const mesh::CellType type
      = cell_entity_type(mesh.topology().cell_type(), dim);
  const int num_vertices = num_cell_vertices(type);

  // Get geometry dofmap and dofs
  const mesh::Geometry& geometry = mesh.geometry();
  const graph::AdjacencyList<std::int32_t>& x_dofs = geometry.dofmap();
  const xt::xtensor<double, 2>& geom_dofs = geometry.x();
  std::vector<double> h_cells(entities.size(), 0);
  assert(num_vertices <= 8);
  xt::xtensor_fixed<double, xt::xshape<8, 3>> points;
  for (std::size_t e = 0; e < entities.size(); ++e)
  {
    // Get the coordinates  of the vertices
    auto dofs = x_dofs.links(entities[e]);
    xt::view(points, xt::range(0, num_vertices), xt::all())
        = xt::view(geom_dofs, xt::keep(dofs), xt::all());

    // Get maximum edge length
    for (int i = 0; i < num_vertices; ++i)
    {
      for (int j = i + 1; j < num_vertices; ++j)
      {
        auto p0 = xt::row(points, i);
        auto p1 = xt::row(points, j);
        h_cells[e] = std::max(h_cells[e], xt::norm_l2(p0 - p1)());
      }
    }
  }

  return h_cells;
}
//-----------------------------------------------------------------------------
xt::xtensor<double, 2>
mesh::cell_normals(const mesh::Mesh& mesh, int dim,
                   const xtl::span<const std::int32_t>& entities)
{
  const int gdim = mesh.geometry().dim();
  const mesh::CellType type
      = mesh::cell_entity_type(mesh.topology().cell_type(), dim);

  // Find geometry nodes for topology entities
  const xt::xtensor<double, 2>& xg = mesh.geometry().x();

  // Orient cells if they are tetrahedron
  bool orient = false;
  if (mesh.topology().cell_type() == mesh::CellType::tetrahedron)
    orient = true;
  xt::xtensor<std::int32_t, 2> geometry_entities
      = entities_to_geometry(mesh, dim, entities, orient);

  const std::size_t num_entities = entities.size();
  xt::xtensor<double, 2> n({num_entities, 3});
  switch (type)
  {
  case mesh::CellType::interval:
  {
    if (gdim > 2)
      throw std::invalid_argument("Interval cell normal undefined in 3D");
    for (std::size_t i = 0; i < num_entities; ++i)
    {
      // Get the two vertices as points
      auto vertices = xt::row(geometry_entities, i);
      auto p0 = xt::row(xg, vertices[0]);
      auto p1 = xt::row(xg, vertices[1]);

      // Define normal by rotating tangent counter-clockwise
      auto t = p1 - p0;
      auto ni = xt::row(n, i);
      ni[0] = -t[1];
      ni[1] = t[0];
      ni[2] = 0.0;
      ni /= xt::norm_l2(ni);
    }
    return n;
  }
  case mesh::CellType::triangle:
  {
    for (std::size_t i = 0; i < num_entities; ++i)
    {
      // Get the three vertices as points
      auto vertices = xt::row(geometry_entities, i);
      auto p0 = xt::row(xg, vertices[0]);
      auto p1 = xt::row(xg, vertices[1]);
      auto p2 = xt::row(xg, vertices[2]);

      // Define cell normal via cross product of first two edges
      auto ni = xt::row(n, i);
      ni = xt::linalg::cross((p1 - p0), (p2 - p0));
      ni /= xt::norm_l2(ni);
    }
    return n;
  }
  case mesh::CellType::quadrilateral:
  {
    // TODO: check
    for (std::size_t i = 0; i < num_entities; ++i)
    {
      // Get three vertices as points
      auto vertices = xt::row(geometry_entities, i);
      auto p0 = xt::row(xg, vertices[0]);
      auto p1 = xt::row(xg, vertices[1]);
      auto p2 = xt::row(xg, vertices[2]);

      // Defined cell normal via cross product of first two edges:
      auto ni = xt::row(n, i);
      ni = xt::linalg::cross((p1 - p0), (p2 - p0));
      ni /= xt::norm_l2(ni);
    }
    return n;
  }
  default:
    throw std::invalid_argument(
        "cell_normal not supported for this cell type.");
  }
}
//-----------------------------------------------------------------------------
xt::xtensor<double, 2>
mesh::midpoints(const mesh::Mesh& mesh, int dim,
                const xtl::span<const std::int32_t>& entities)
{
  const xt::xtensor<double, 2>& x = mesh.geometry().x();

  // Build map from entity -> geometry dof
  // FIXME: This assumes a linear geometry.
  xt::xtensor<std::int32_t, 2> entity_to_geometry
      = entities_to_geometry(mesh, dim, entities, false);

  xt::xtensor<double, 2> x_mid({entities.size(), 3});
  for (std::size_t e = 0; e < entity_to_geometry.shape(0); ++e)
  {
    auto rows = xt::row(entity_to_geometry, e);
    xt::row(x_mid, e) = xt::mean(xt::view(x, xt::keep(rows)), 0);
  }

  return x_mid;
}
//-----------------------------------------------------------------------------
std::vector<std::int32_t> mesh::locate_entities(
    const mesh::Mesh& mesh, int dim,
    const std::function<xt::xtensor<bool, 1>(const xt::xtensor<double, 2>&)>&
        marker)
{
  const mesh::Topology& topology = mesh.topology();
  const int tdim = topology.dim();

  // Create entities and connectivities
  mesh.topology_mutable().create_entities(dim);
  mesh.topology_mutable().create_connectivity(tdim, 0);
  if (dim < tdim)
    mesh.topology_mutable().create_connectivity(dim, 0);

  // Get all vertex 'node' indices
  const graph::AdjacencyList<std::int32_t>& x_dofmap = mesh.geometry().dofmap();
  const std::int32_t num_vertices = topology.index_map(0)->size_local()
                                    + topology.index_map(0)->num_ghosts();
  auto c_to_v = topology.connectivity(tdim, 0);
  assert(c_to_v);
  std::vector<std::int32_t> vertex_to_node(num_vertices);
  for (int c = 0; c < c_to_v->num_nodes(); ++c)
  {
    auto x_dofs = x_dofmap.links(c);
    auto vertices = c_to_v->links(c);
    for (std::size_t i = 0; i < vertices.size(); ++i)
      vertex_to_node[vertices[i]] = x_dofs[i];
  }

  // Pack coordinates of vertices
  const xt::xtensor<double, 2>& x_nodes = mesh.geometry().x();
  xt::xtensor<double, 2> x_vertices({3, vertex_to_node.size()});
  for (std::size_t i = 0; i < vertex_to_node.size(); ++i)
    for (std::size_t j = 0; j < 3; ++j)
      x_vertices(j, i) = x_nodes(vertex_to_node[i], j);

  // Run marker function on vertex coordinates
  const xt::xtensor<bool, 1> marked = marker(x_vertices);
  if (marked.shape(0) != x_vertices.shape(1))
    throw std::runtime_error("Length of array of markers is wrong.");

  // Iterate over entities to build vector of marked entities
  auto e_to_v = topology.connectivity(dim, 0);
  assert(e_to_v);
  std::vector<std::int32_t> entities;
  for (int e = 0; e < e_to_v->num_nodes(); ++e)
  {
    // Iterate over entity vertices
    bool all_vertices_marked = true;
    for (std::int32_t v : e_to_v->links(e))
    {
      if (!marked[v])
      {
        all_vertices_marked = false;
        break;
      }
    }

    if (all_vertices_marked)
      entities.push_back(e);
  }

  return entities;
}
//-----------------------------------------------------------------------------
std::vector<std::int32_t> mesh::locate_entities_boundary(
    const mesh::Mesh& mesh, int dim,
    const std::function<xt::xtensor<bool, 1>(const xt::xtensor<double, 2>&)>&
        marker)
{
  const mesh::Topology& topology = mesh.topology();
  const int tdim = topology.dim();
  if (dim == tdim)
  {
    throw std::runtime_error(
        "Cannot use mesh::locate_entities_boundary (boundary) for cells.");
  }

  // Compute marker for boundary facets
  mesh.topology_mutable().create_entities(tdim - 1);
  mesh.topology_mutable().create_connectivity(tdim - 1, tdim);
  const std::vector boundary_facet = mesh::compute_boundary_facets(topology);

  // Create entities and connectivities
  mesh.topology_mutable().create_entities(dim);
  mesh.topology_mutable().create_connectivity(tdim - 1, dim);
  mesh.topology_mutable().create_connectivity(tdim - 1, 0);
  mesh.topology_mutable().create_connectivity(0, tdim);
  mesh.topology_mutable().create_connectivity(tdim, 0);

  // Build set of vertices on boundary and set of boundary entities
  auto f_to_v = topology.connectivity(tdim - 1, 0);
  assert(f_to_v);
  auto f_to_e = topology.connectivity(tdim - 1, dim);
  assert(f_to_e);
  std::unordered_set<std::int32_t> boundary_vertices;
  std::unordered_set<std::int32_t> facet_entities;
  for (std::size_t f = 0; f < boundary_facet.size(); ++f)
  {
    if (boundary_facet[f])
    {
      for (auto e : f_to_e->links(f))
        facet_entities.insert(e);

      for (auto v : f_to_v->links(f))
        boundary_vertices.insert(v);
    }
  }

  // Get geometry data
  const graph::AdjacencyList<std::int32_t>& x_dofmap = mesh.geometry().dofmap();
  const xt::xtensor<double, 2>& x_nodes = mesh.geometry().x();

  // Build vector of boundary vertices
  const std::vector<std::int32_t> vertices(boundary_vertices.begin(),
                                           boundary_vertices.end());

  // Get all vertex 'node' indices
  auto v_to_c = topology.connectivity(0, tdim);
  assert(v_to_c);
  auto c_to_v = topology.connectivity(tdim, 0);
  assert(c_to_v);
  xt::xtensor<double, 2> x_vertices({3, vertices.size()});
  std::vector<std::int32_t> vertex_to_pos(v_to_c->num_nodes(), -1);
  for (std::size_t i = 0; i < vertices.size(); ++i)
  {
    const std::int32_t v = vertices[i];

    // Get first cell and find position
    const int c = v_to_c->links(v)[0];
    auto vertices = c_to_v->links(c);
    auto it = std::find(vertices.begin(), vertices.end(), v);
    assert(it != vertices.end());
    const int local_pos = std::distance(vertices.begin(), it);

    auto dofs = x_dofmap.links(c);
    for (int j = 0; j < 3; ++j)
      x_vertices(j, i) = x_nodes(dofs[local_pos], j);

    vertex_to_pos[v] = i;
  }

  // Run marker function on the vertex coordinates
  const xt::xtensor<bool, 1> marked = marker(x_vertices);
  if (marked.shape(0) != x_vertices.shape(1))
    throw std::runtime_error("Length of array of markers is wrong.");

  // Loop over entities and check vertex markers
  auto e_to_v = topology.connectivity(dim, 0);
  assert(e_to_v);
  std::vector<std::int32_t> entities;
  for (auto e : facet_entities)
  {
    // Assume all vertices on this entity are marked
    bool all_vertices_marked = true;

    // Iterate over entity vertices
    for (auto v : e_to_v->links(e))
    {
      const std::int32_t pos = vertex_to_pos[v];
      if (!marked[pos])
      {
        all_vertices_marked = false;
        break;
      }
    }

    // Mark facet with all vertices marked
    if (all_vertices_marked)
      entities.push_back(e);
  }

  return entities;
}
//-----------------------------------------------------------------------------
xt::xtensor<std::int32_t, 2>
mesh::entities_to_geometry(const mesh::Mesh& mesh, int dim,
                           const xtl::span<const std::int32_t>& entity_list,
                           bool orient)
{
  dolfinx::mesh::CellType cell_type = mesh.topology().cell_type();
  const std::size_t num_entity_vertices
      = mesh::num_cell_vertices(mesh::cell_entity_type(cell_type, dim));
  xt::xtensor<std::int32_t, 2> entity_geometry(
      {entity_list.size(), num_entity_vertices});

  if (orient
      and (cell_type != dolfinx::mesh::CellType::tetrahedron or dim != 2))
  {
    throw std::runtime_error("Can only orient facets of a tetrahedral mesh");
  }

  const mesh::Geometry& geometry = mesh.geometry();
  const xt::xtensor<double, 2>& geom_dofs = geometry.x();
  const mesh::Topology& topology = mesh.topology();

  const int tdim = topology.dim();
  mesh.topology_mutable().create_entities(dim);
  mesh.topology_mutable().create_connectivity(dim, tdim);
  mesh.topology_mutable().create_connectivity(dim, 0);
  mesh.topology_mutable().create_connectivity(tdim, 0);

  const graph::AdjacencyList<std::int32_t>& xdofs = geometry.dofmap();
  const auto e_to_c = topology.connectivity(dim, tdim);
  assert(e_to_c);
  const auto e_to_v = topology.connectivity(dim, 0);
  assert(e_to_v);
  const auto c_to_v = topology.connectivity(tdim, 0);
  assert(c_to_v);
  for (std::size_t i = 0; i < entity_list.size(); ++i)
  {
    const std::int32_t idx = entity_list[i];
    const std::int32_t cell = e_to_c->links(idx)[0];
    auto ev = e_to_v->links(idx);
    assert(ev.size() == num_entity_vertices);
    const auto cv = c_to_v->links(cell);
    const auto xc = xdofs.links(cell);
    for (std::size_t j = 0; j < num_entity_vertices; ++j)
    {
      int k = std::distance(cv.begin(), std::find(cv.begin(), cv.end(), ev[j]));
      assert(k < (int)cv.size());
      entity_geometry(i, j) = xc[k];
    }

    if (orient)
    {
      // Compute cell midpoint
      xt::xtensor_fixed<double, xt::xshape<3>> midpoint = {0, 0, 0};
      for (std::int32_t j : xc)
        for (int k = 0; k < 3; ++k)
          midpoint[k] += geom_dofs(j, k);
      midpoint /= xc.size();

      // Compute vector triple product of two edges and vector to midpoint
      auto p0 = xt::row(geom_dofs, entity_geometry(i, 0));
      auto p1 = xt::row(geom_dofs, entity_geometry(i, 1));
      auto p2 = xt::row(geom_dofs, entity_geometry(i, 2));

      xt::xtensor_fixed<double, xt::xshape<3, 3>> a;
      xt::row(a, 0) = midpoint - p0;
      xt::row(a, 1) = p1 - p0;
      xt::row(a, 2) = p2 - p0;

      // Midpoint direction should be opposite to normal, hence this
      // should be negative. Switch points if not.
      if (xt::linalg::det(a) > 0.0)
        std::swap(entity_geometry(i, 1), entity_geometry(i, 2));
    }
  }

  return entity_geometry;
}
//------------------------------------------------------------------------
std::vector<std::int32_t> mesh::exterior_facet_indices(const Mesh& mesh)
{
  // Note: Possible duplication of mesh::Topology::compute_boundary_facets

  const mesh::Topology& topology = mesh.topology();
  std::vector<std::int32_t> surface_facets;

  // Get number of facets owned by this process
  const int tdim = topology.dim();
  mesh.topology_mutable().create_connectivity(tdim - 1, tdim);
  auto f_to_c = topology.connectivity(tdim - 1, tdim);
  assert(topology.index_map(tdim - 1));
  std::set<std::int32_t> fwd_shared_facets;

  // Only need to consider shared facets when there are no ghost cells
  if (topology.index_map(tdim)->num_ghosts() == 0)
  {
    fwd_shared_facets.insert(
        topology.index_map(tdim - 1)->shared_indices().array().begin(),
        topology.index_map(tdim - 1)->shared_indices().array().end());
  }

  // Find all owned facets (not ghost) with only one attached cell, which are
  // also not shared forward (ghost on another process)
  const int num_facets = topology.index_map(tdim - 1)->size_local();
  for (int f = 0; f < num_facets; ++f)
  {
    if (f_to_c->num_links(f) == 1
        and fwd_shared_facets.find(f) == fwd_shared_facets.end())
    {
      surface_facets.push_back(f);
    }
  }

  return surface_facets;
}
//------------------------------------------------------------------------------
graph::AdjacencyList<std::int32_t>
mesh::partition_cells_graph(MPI_Comm comm, int n, int tdim,
                            const graph::AdjacencyList<std::int64_t>& cells,
                            mesh::GhostMode ghost_mode)
{
  return partition_cells_graph(comm, n, tdim, cells, ghost_mode,
                               &graph::partition_graph);
}
//-----------------------------------------------------------------------------
graph::AdjacencyList<std::int32_t>
mesh::partition_cells_graph(MPI_Comm comm, int n, int tdim,
                            const graph::AdjacencyList<std::int64_t>& cells,
                            mesh::GhostMode ghost_mode,
                            const graph::partition_fn& partfn)
{
  LOG(INFO) << "Compute partition of cells across ranks";

  // Compute distributed dual graph (for the cells on this process)
  const auto [dual_graph, graph_info]
      = mesh::build_dual_graph(comm, cells, tdim);

  // Extract data from graph_info
  const auto [num_ghost_nodes, num_local_edges] = graph_info;

  // Just flag any kind of ghosting for now
  bool ghosting = (ghost_mode != mesh::GhostMode::none);

  // Compute partition
  return partfn(comm, n, dual_graph, num_ghost_nodes, ghosting);
}
//-----------------------------------------------------------------------------
mesh::Mesh mesh::add_ghost_layer(const mesh::Mesh& mesh)
{
  MPI_Comm comm = mesh.mpi_comm();
  int mpi_rank = dolfinx::MPI::rank(comm);

  // Get topology information
  const mesh::Topology& topology = mesh.topology();
  int tdim = topology.dim();
  auto fv = topology.connectivity(tdim - 1, 0);
  auto vc = topology.connectivity(0, tdim);
  auto cv = topology.connectivity(tdim, 0);
  auto map_v = topology.index_map(0);
  auto map_c = topology.index_map(tdim);

  // Implemented in three steps:
  // FIXME: Add description ...

  // TODO: Profile this function ...

  // Data shared between steps
  std::vector<std::int64_t> recv_data;
  std::vector<int> recv_sizes;
  std::vector<int> recv_disp;

  // Step 1: Identify interface entities and send information to the entity
  // owner.
  {
    std::vector<bool> bnd_facets = mesh::compute_interface_facets(topology);
    std::vector<std::int32_t> facet_indices;

    // Get indices of interface facets
    for (std::size_t f = 0; f < bnd_facets.size(); ++f)
      if (bnd_facets[f])
        facet_indices.push_back(f);

    // Identify ghost interface vertices
    std::int32_t local_size = map_v->size_local();
    std::vector<std::int32_t> int_vertices;
    int_vertices.reserve(facet_indices.size() * 2);
    for (const auto& f : facet_indices)
      for (const auto& v : fv->links(f))
        if (v >= local_size)
          int_vertices.push_back(v);

    // Remove repeated vertices
    std::sort(int_vertices.begin(), int_vertices.end());
    int_vertices.erase(std::unique(int_vertices.begin(), int_vertices.end()),
                       int_vertices.end());

    // Compute the global indices of the vertices on the interface
    std::vector<std::int64_t> int_vertices_global(int_vertices.size());
    map_v->local_to_global(int_vertices, int_vertices_global);

    // Get the owners of each interface vertex
    auto ghost_owners = map_v->ghost_owner_rank();
    auto ghosts = map_v->ghosts();
    std::vector<std::int32_t> owner(int_vertices_global.size());
    // FIXME: This could be made faster if ghosts were sorted
    for (std::size_t i = 0; i < int_vertices_global.size(); i++)
    {
      std::int64_t ghost = int_vertices_global[i];
      auto it = std::find(ghosts.begin(), ghosts.end(), ghost);
      assert(it != ghosts.end());
      int pos = std::distance(ghosts.begin(), it);
      owner[i] = ghost_owners[pos];
    }

    // Each process reports to the owners of the vertices it has on
    // its boundary. Reverse_comm: Ghost -> owner communication

    // Figure out how much data to send to each neighbor (ghost owner).
    MPI_Comm reverse_comm = map_v->comm(common::IndexMap::Direction::reverse);
    auto [sources, destinations] = dolfinx::MPI::neighbors(reverse_comm);
    std::vector<int> send_sizes(destinations.size(), 0);
    recv_sizes.resize(sources.size(), 0);
    for (std::size_t i = 0; i < int_vertices_global.size(); i++)
    {
      auto it = std::find(destinations.begin(), destinations.end(), owner[i]);
      assert(it != destinations.end());
      int pos = std::distance(destinations.begin(), it);
      send_sizes[pos]++;
    }

    MPI_Neighbor_alltoall(send_sizes.data(), 1, MPI_INT, recv_sizes.data(), 1,
                          MPI_INT, reverse_comm);

    // Prepare communication displacements
    std::vector<int> send_disp(destinations.size() + 1, 0);
    recv_disp.resize(sources.size() + 1, 0);
    std::partial_sum(send_sizes.begin(), send_sizes.end(),
                     send_disp.begin() + 1);
    std::partial_sum(recv_sizes.begin(), recv_sizes.end(),
                     recv_disp.begin() + 1);

    // Pack the data to send the owning rank:
    // Each process send its interface vertices to the respective owner
    std::vector<std::int64_t> send_data(send_disp.back());
    recv_data.resize(recv_disp.back());
    std::vector<int> insert_pos = send_disp;
    for (std::size_t i = 0; i < int_vertices_global.size(); i++)
    {
      auto it = std::find(destinations.begin(), destinations.end(), owner[i]);
      assert(it != destinations.end());
      int p = std::distance(destinations.begin(), it);
      int& pos = insert_pos[p];
      send_data[pos++] = int_vertices_global[i];
    }

    // A rank in the neighborhood communicator can have no incoming or
    // outcoming edges. This may cause OpenMPI to crash. Workaround:
    send_sizes.reserve(1);
    recv_sizes.reserve(1);
    MPI_Neighbor_alltoallv(send_data.data(), send_sizes.data(),
                           send_disp.data(), MPI_INT64_T, recv_data.data(),
                           recv_sizes.data(), recv_disp.data(), MPI_INT64_T,
                           reverse_comm);

    // recv_data should be equal to map_v.shared_indices if the if the original
    // mesh does not have ghosts cells.
  }

  graph::AdjacencyList<std::int32_t> dest(0);

  // Step 2: Each process now has a list of all processes for which one of its
  // owned vertices is an interface vertice. Gather information an send the
  // list to all processes that share the same vertice.
  {
    MPI_Comm forward_comm = map_v->comm(common::IndexMap::Direction::forward);
    auto [sources, destinations] = dolfinx::MPI::neighbors(forward_comm);

    // Pack information into a more manageable format
    std::map<std::int64_t, std::vector<int>> vertex_procs;
    for (std::size_t i = 0; i < recv_sizes.size(); i++)
      for (int j = recv_disp[i]; j < recv_disp[i + 1]; j++)
        vertex_procs[recv_data[j]].push_back(i);

    // Figure out how much data to send to each neighbor
    // For every shared vertice we send:
    // [Global index, Number of Processes, P1, ...,  PN]
    std::vector<int> send_sizes(destinations.size(), 0);
    recv_sizes.resize(sources.size(), 0);
    for (auto const& [vertex, neighbors] : vertex_procs)
      for (const int& p : neighbors)
        send_sizes[p] += (2 + neighbors.size() + 1);

    MPI_Neighbor_alltoall(send_sizes.data(), 1, MPI_INT, recv_sizes.data(), 1,
                          MPI_INT, forward_comm);

    // Prepare communication displacements
    std::vector<int> send_disp(destinations.size() + 1, 0);
    recv_disp.resize(sources.size() + 1, 0);
    std::partial_sum(send_sizes.begin(), send_sizes.end(),
                     send_disp.begin() + 1);
    std::partial_sum(recv_sizes.begin(), recv_sizes.end(),
                     recv_disp.begin() + 1);

    // Pack the data to send: EG
    // [V100 3 P1 P2 P3 V2 2 P2 P3 ...]
    std::vector<std::int64_t> send_data(send_disp.back());
    std::vector<std::int64_t> recv_data(recv_disp.back());
    std::vector<int> insert_pos = send_disp;
    for (auto const& [vertex, neighbors] : vertex_procs)
    {
      for (auto p : neighbors)
      {
        send_data[insert_pos[p]++] = vertex;
        // Should include this process to the list (+1) as the
        // vertex owner.
        send_data[insert_pos[p]++] = neighbors.size() + 1;
        send_data[insert_pos[p]++] = mpi_rank;
        for (auto other : neighbors)
          send_data[insert_pos[p]++] = destinations[other];
      }
    }

    // Translate from neighbor rank to rank in global communicator
    // and add the current rank to the list of connected procs via
    // vertex
    for (auto& [vertex, neighbors] : vertex_procs)
    {
      for (auto& p : neighbors)
        p = destinations[p];
      neighbors.push_back(mpi_rank);
    }

    // A rank in the neighborhood communicator can have no incoming or
    // outcoming edges. This may cause OpenMPI to crash. Workaround:
    if (send_sizes.empty())
      send_sizes.reserve(1);
    if (recv_sizes.empty())
      recv_sizes.reserve(1);

    // Send packaged information only to relevant neighbors
    MPI_Neighbor_alltoallv(send_data.data(), send_sizes.data(),
                           send_disp.data(), MPI_INT64_T, recv_data.data(),
                           recv_sizes.data(), recv_disp.data(), MPI_INT64_T,
                           forward_comm);

    // Unpack received data and add to the vertex_procs map
    auto vc = topology.connectivity(0, tdim);
    for (auto it = recv_data.begin(); it < recv_data.end();)
    {
      const std::int64_t global_index = *it++;
      int num_procs = *it++;
      auto& processes = vertex_procs[global_index];
      std::copy_n(it, num_procs, std::back_inserter(processes));
      std::advance(it, num_procs);
    }

    std::vector<std::int64_t> global_indices(vertex_procs.size());
    std::transform(vertex_procs.begin(), vertex_procs.end(),
                   global_indices.begin(),
                   [](auto& pair) { return pair.first; });

    std::vector<std::int32_t> local_indices(global_indices.size());
    map_v->global_to_local(global_indices, local_indices);

    // Start getting the destination of local cells
    std::int32_t num_local_cells = map_c->size_local();
    std::vector<std::int32_t> num_dest(num_local_cells, 1);

    // // Some cells should be already shared
    // const graph::AdjacencyList<std::int32_t> shared_cells
    //     = map_c->shared_indices();

    // // Transpose Adjacency list, (cell - list procs)
    // std::vector<std::int32_t> counter(num_local_cells, 0);
    // graph::AdjacencyList<std::int32_t> cells_sharing_procs(0);
    // if (shared_cells.num_nodes() != 0)
    // {
    //   std::int32_t num_nodes = shared_cells.num_nodes();
    //   for (std::int32_t p = 0; p < num_nodes; p++)
    //     for (auto cell : shared_cells.links(p))
    //       counter[cell]++;

    //   std::vector<std::int32_t> offsets(counter.size() + 1, 0);
    //   std::partial_sum(counter.begin(), counter.end(), offsets.begin() + 1);

    //   std::vector<std::int32_t> data(offsets.back());
    //   std::vector<std::int32_t> insert_pos = offsets;

    //   for (std::int32_t p = 0; p < num_nodes; p++)
    //     for (const auto& cell : shared_cells.links(p))
    //       data[insert_pos[cell]++] = destinations[p];

    //   cells_sharing_procs = graph::AdjacencyList<std::int32_t>(
    //       std::move(data), std::move(offsets));
    // }

    // Get number of destinations from vertex connectivity
    int i = 0;
    for (auto const& [vertex, neighbors] : vertex_procs)
      for (const auto& cell : vc->links(local_indices[i++]))
        if (cell < num_local_cells)
          num_dest[cell] += neighbors.size();

    // // Get number of destinations from previously shared cells
    // for (std::int32_t c = 0; c < cells_sharing_procs.num_nodes(); c++)
    //   num_dest[c] += cells_sharing_procs.num_links(c);

    // Calculate extended offsets (including repeated entries)
    std::vector<std::int32_t> ext_offsets(num_dest.size() + 1);
    std::partial_sum(num_dest.begin(), num_dest.end(), ext_offsets.begin() + 1);
    std::vector<std::int32_t> data(ext_offsets.back(), mpi_rank);
    insert_pos = ext_offsets;

    // Get destinations for each cell
    i = 0;
    for (auto const& [vertex, neighbors] : vertex_procs)
    {
      std::int32_t local_index = local_indices[i++];
      for (const auto& cell : vc->links(local_index))
        if (cell < num_local_cells)
        {
          std::copy(neighbors.begin(), neighbors.end(),
                    data.begin() + insert_pos[cell]);
          insert_pos[cell] += neighbors.size();
        }
    }

    // for (std::int32_t c = 0; c < cells_sharing_procs.num_nodes(); c++)
    // {
    //   const auto& procs = cells_sharing_procs.links(c);
    //   std::copy(procs.begin(), procs.end(), data.begin() + insert_pos[c]);
    //   insert_pos[c] += procs.size();
    // }

    // Create destination adjacency list with duplicated entries
    graph::AdjacencyList<std::int32_t> dest_duplicates(std::move(data),
                                                       std::move(ext_offsets));

    // Remove duplicates entries in the destination Adjacency List
    std::vector<std::int32_t> cell_data;
    std::fill(num_dest.begin(), num_dest.end(), 0);
    for (std::int32_t c = 0; c < num_local_cells; c++)
    {
      // unordered_set is potentially faster, but data is not ordered
      std::set<std::int32_t> local_set(dest_duplicates.links(c).begin(),
                                       dest_duplicates.links(c).end());
      // Remove the current rank from local_set, and add insert it in the
      // first position cell_data (the current rank is the owner of the cell).
      local_set.erase(mpi_rank);
      cell_data.push_back(mpi_rank);
      cell_data.insert(cell_data.end(), local_set.begin(), local_set.end());
      num_dest[c] = local_set.size() + 1;
    }

    std::vector<std::int32_t> offsets(num_dest.size() + 1, 0);
    std::partial_sum(num_dest.begin(), num_dest.end(), offsets.begin() + 1);
    dest = graph::AdjacencyList<std::int32_t>(cell_data, offsets);
  }

  // Step 3: Create new mesh from local data
  auto partitioner = [&dest](...) { return dest; };

  const auto& geometry = mesh.geometry();

  std::int32_t num_local_cells = map_c->size_local();
  std::int32_t num_cells = map_c->size_local() + map_c->num_ghosts();
  std::vector<std::int32_t> vertex_to_x(map_v->size_local()
                                        + map_v->num_ghosts());
  for (int c = 0; c < num_cells; ++c)
  {
    auto vertices = cv->links(c);
    auto dofs = geometry.dofmap().links(c);
    for (std::size_t i = 0; i < vertices.size(); ++i)
      vertex_to_x[vertices[i]] = dofs[i];
  }

  // FIXME: Allocate data to avoid insert/push_back
  std::vector<std::int64_t> topology_array;
  std::vector<int> counter(num_local_cells);
  for (std::int32_t i = 0; i < num_local_cells; i++)
  {
    std::vector<int64_t> global_inds(cv->num_links(i));
    map_v->local_to_global(cv->links(i), global_inds);
    topology_array.insert(topology_array.end(), global_inds.begin(),
                          global_inds.end());
    counter[i] += global_inds.size();
  }

  std::vector<std::int32_t> offsets(counter.size() + 1, 0);
  std::partial_sum(counter.begin(), counter.end(), offsets.begin() + 1);
  graph::AdjacencyList<std::int64_t> cell_vertices(topology_array, offsets);

  // Copy over existing mesh vertices
  const std::int32_t num_vertices = map_v->size_local();
  const array2d<double>& x_g = geometry.x();
  int gdim = geometry.dim();
  array2d<double> x(num_vertices, gdim);
  for (int v = 0; v < num_vertices; ++v)
    for (int j = 0; j < gdim; ++j)
      x(v, j) = x_g(vertex_to_x[v], j);

  return mesh::create_mesh(mesh.mpi_comm(), cell_vertices, geometry.cmap(), x,
                           mesh::GhostMode::shared_facet, partitioner);
}
//-----------------------------------------------------------------------------