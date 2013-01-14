// Copyright (C) 2012 Chris Richardson
// 
// This file is part of DOLFIN.
// 
// DOLFIN is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// DOLFIN is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Lesser General Public License for more details.
// 
// You should have received a copy of the GNU Lesser General Public License
// along with DOLFIN. If not, see <http://www.gnu.org/licenses/>.
// 
// 
// First Added: 2012-12-19
// Last Changed: 2013-01-14

#include <vector>
#include <map>
#include <boost/unordered_map.hpp>
#include <boost/multi_array.hpp>

#include <dolfin/common/types.h>
#include <dolfin/common/MPI.h>
#include <dolfin/common/Timer.h>
#include <dolfin/mesh/Mesh.h>
#include <dolfin/mesh/Cell.h>
#include <dolfin/mesh/Edge.h>
#include <dolfin/mesh/Face.h>
#include <dolfin/mesh/Vertex.h>
#include <dolfin/mesh/LocalMeshData.h>

#include <dolfin/refinement/ParallelRefinement.h>

#include "ParallelRefinement3D.h"

using namespace dolfin;

//-----------------------------------------------------------------------------
void ParallelRefinement3D::refine(Mesh& new_mesh, const Mesh& mesh,
                                  const MeshFunction<bool>& refinement_marker)
{
  std::size_t tdim = mesh.topology().dim();
  
  if(refinement_marker.dim() != tdim)
  {
    dolfin_error("ParallelRefinement3D.cpp",
                 "mark edges",
                 "Only Cell based markers are supported at present");
  }
  
  // Ensure connectivity from cells to edges
  mesh.init(1);
  mesh.init(1, tdim);

  ParallelRefinement p(mesh);

  // Mark all edges of marked cells
  p.mark(refinement_marker);

  std::size_t update_count = 1;
  
  while (update_count != 0)
  {
    update_count = 0;
    
    // Transmit shared marked edges
    p.update_logical_edgefunction();

    for(CellIterator cell(mesh); !cell.end(); ++cell)
    {
      std::size_t n_marked = p.marked_edge_count(*cell);
      
      // If more than 3 edges are already marked, mark all
      if (n_marked == 4 || n_marked == 5)
      { 
        p.mark(*cell);
        update_count++;
      }

      // With 3 marked edges, they must be all on the same face, otherwise, just mark all
      if (n_marked == 3)
      {
        std::size_t nmax = 0;
        for (FaceIterator face(*cell); !face.end(); ++face)
        {
          const std::size_t n_face = p.marked_edge_count(*face);
          nmax = (n_face > nmax) ? n_face : nmax;
        }        
        if(nmax != 3)
        {
          p.mark(*cell);
          update_count++;
        }
      }
    }
    update_count = MPI::sum(update_count);
  }
  
  // All cells now have either 0, 1, 2, 3* or 6 edges marked.
  // * (3 are all on the same face)

  // Create new vertices
  p.create_new_vertices();
  std::map<std::size_t, std::size_t>& edge_to_new_vertex = p.edge_to_new_vertex();
  
  // Create new topology

  for(CellIterator cell(mesh); !cell.end(); ++cell)
  {    
    VertexIterator v(*cell);
    EdgeIterator e(*cell);

    std::vector<std::size_t> marked_edges;
    for(std::size_t j = 0 ; j < 6 ; ++j)
    {
      if (p.is_marked(e[j].index()))
        marked_edges.push_back(j);
    }

    if(marked_edges.size() == 0) //straight copy of cell (1->1)
    {
      const std::size_t v0 = v[0].global_index();
      const std::size_t v1 = v[1].global_index();
      const std::size_t v2 = v[2].global_index();
      const std::size_t v3 = v[3].global_index();
      p.new_cell(v0, v1, v2, v3);
    }
    else if(marked_edges.size() == 1) // "green" refinement (bisection)
    {
      const std::size_t new_edge = marked_edges[0];
      const std::size_t v_new = edge_to_new_vertex[e[new_edge].index()];
      VertexIterator vn(e[new_edge]);
      const std::size_t v_near_0 = vn[0].global_index();
      const std::size_t v_near_1 = vn[1].global_index();
      // opposite edges always add up to 5
      const std::size_t opp_edge = 5 - new_edge;
      VertexIterator vf(e[opp_edge]);
      const std::size_t v_far_0 = vf[0].global_index();
      const std::size_t v_far_1 = vf[1].global_index();

      p.new_cell(v_far_0, v_far_1, v_new, v_near_0);
      p.new_cell(v_far_0, v_far_1, v_new, v_near_1);
    }
    else if(marked_edges.size() == 2) 
    {
      const std::size_t new_edge_0 = marked_edges[0];
      const std::size_t new_edge_1 = marked_edges[1];
      const std::size_t e0 = edge_to_new_vertex[e[new_edge_0].index()];
      const std::size_t e1 = edge_to_new_vertex[e[new_edge_1].index()];
      VertexIterator v0(e[new_edge_0]);
      VertexIterator v1(e[new_edge_1]);

      // Opposite edges add up to 5
      // This is effectively a double bisection
      if( (new_edge_0 + new_edge_1) == 5)
      {
        const std::size_t e0v0 = v0[0].global_index();
        const std::size_t e0v1 = v0[1].global_index();
        const std::size_t e1v0 = v1[0].global_index();
        const std::size_t e1v1 = v1[1].global_index();
        
        p.new_cell(e0, e1, e0v0, e1v0);
        p.new_cell(e0, e1, e0v1, e1v0);
        p.new_cell(e0, e1, e0v0, e1v1);
        p.new_cell(e0, e1, e0v1, e1v1);
      }
      else // Both edges on same face
      {
        // Find shared and non-shared vertices
        std::size_t v_common, v_leg_0, v_leg_1;      
        double d0, d1;
        
        for(std::size_t i = 0; i < 2; ++i)
          for(std::size_t j = 0; j < 2; ++j)
          {
            if(v0[i] == v1[j])
            {
              v_common = v0[i].global_index();
              v_leg_0 = v0[1-i].global_index();
              v_leg_1 = v1[1-j].global_index();

              // Find distance across trapezoid
              const Point p_leg_0 = v0[1-i].point();
              const Point p_leg_1 = v1[1-j].point();
              d0 = p_leg_0.distance(e[new_edge_1].midpoint());
              d1 = p_leg_1.distance(e[new_edge_0].midpoint());
            }
          }
        
        // Need to find the 'uncommon' vertex of the two edges
        // which is furthest from both
        std::size_t v_far;
      
        for(std::size_t i = 0; i < 4;++i)
        {
          const std::size_t v_i = v[i].global_index();
          if(v_i != v_common && 
             v_i != v_leg_0 &&
             v_i != v_leg_1)
          {
            v_far = v_i;
          }
        }

        // Add 'top cell' always the same
        p.new_cell(v_far, v_common, e0, e1);
      
        // Choose bottom cell consistently
        if(d0 > d1 || (d0 == d1 && v_leg_0 > v_leg_1)) 
        {
          p.new_cell(v_far, e0, e1, v_leg_1);
          p.new_cell(v_far, e0, v_leg_0, v_leg_1);
        }
        else 
        {
          p.new_cell(v_far, e1, e0, v_leg_0);
          p.new_cell(v_far, e1, v_leg_1, v_leg_0);
        }
      }
    }
    else if(marked_edges.size() == 3) // refinement of one face into 4 triangles
    {
      // Assumes edges are on one face - will break otherwise
      VertexIterator v0(e[marked_edges[0]]);
      VertexIterator v1(e[marked_edges[1]]);
      VertexIterator v2(e[marked_edges[2]]);
      const std::size_t e0 = edge_to_new_vertex[e[marked_edges[0]].index()];
      const std::size_t e1 = edge_to_new_vertex[e[marked_edges[1]].index()];
      const std::size_t e2 = edge_to_new_vertex[e[marked_edges[2]].index()];

      std::size_t v01, v12, v20, v_far;
      
      for(std::size_t i = 0; i < 2; ++i)
        for(std::size_t j = 0; j < 2; ++j)
        {
          if(v0[i]==v1[j])
            v01=v0[i].global_index();

          if(v1[i]==v2[j])
            v12=v1[i].global_index();

          if(v2[i]==v0[j])
            v20=v2[i].global_index();
        }

      for(std::size_t i = 0; i<4 ;++i)
      {
        const std::size_t v_i=v[i].global_index();
        if(v_i != v01 
        && v_i != v12
        && v_i != v20)
        {
          v_far = v_i;
        }
        
      }
      
      p.new_cell(v_far, e0, e1, e2);
      p.new_cell(v_far, e0, v01, e1);
      p.new_cell(v_far, e1, v12, e2);
      p.new_cell(v_far, e2, v20, e0);

    }
    else if(marked_edges.size() == 6)
    {
      const std::size_t v0 = v[0].global_index();
      const std::size_t v1 = v[1].global_index();
      const std::size_t v2 = v[2].global_index();
      const std::size_t v3 = v[3].global_index();

      const std::size_t e0 = edge_to_new_vertex[e[0].index()];
      const std::size_t e1 = edge_to_new_vertex[e[1].index()];
      const std::size_t e2 = edge_to_new_vertex[e[2].index()];
      const std::size_t e3 = edge_to_new_vertex[e[3].index()];
      const std::size_t e4 = edge_to_new_vertex[e[4].index()];
      const std::size_t e5 = edge_to_new_vertex[e[5].index()];

      p.new_cell(v0, e3, e4, e5);
      p.new_cell(v1, e1, e2, e5);
      p.new_cell(v2, e0, e2, e4);
      p.new_cell(v3, e0, e1, e3);

      const Point p0 = e[0].midpoint();
      const Point p1 = e[1].midpoint();
      const Point p2 = e[2].midpoint();
      const Point p3 = e[3].midpoint();
      const Point p4 = e[4].midpoint();
      const Point p5 = e[5].midpoint();
      const double d05 = p0.distance(p5);
      const double d14 = p1.distance(p4);
      const double d23 = p2.distance(p3);
      
      // Then divide the remaining octahedron into 4 tetrahedra
      if (d05 <= d14 && d14 <= d23)
      {
        p.new_cell(e0, e1, e2, e5);
        p.new_cell(e0, e1, e3, e5);
        p.new_cell(e0, e2, e4, e5);
        p.new_cell(e0, e3, e4, e5);
      }
      else if (d14 <= d23)
      {
        p.new_cell(e0, e1, e2, e4);
        p.new_cell(e0, e1, e3, e4);
        p.new_cell(e1, e2, e4, e5);
        p.new_cell(e1, e3, e4, e5);
      }
      else
      {
        p.new_cell(e0, e1, e2, e3);
        p.new_cell(e0, e2, e3, e4);
        p.new_cell(e1, e2, e3, e5);
        p.new_cell(e2, e3, e4, e5);
      }
      
    }
  }
  
  p.partition(new_mesh);
}
//-----------------------------------------------------------------------------
void ParallelRefinement3D::refine(Mesh& new_mesh, const Mesh& mesh)
{
  if(MPI::num_processes()==1)
  {
    dolfin_error("ParallelRefinement3D.cpp",
                 "refine mesh",
                 "Only works in parallel");
  }

  const uint tdim = mesh.topology().dim();
  const uint gdim = mesh.geometry().dim();

  if(tdim != 3 || gdim != 3)
  {
    dolfin_error("ParallelRefinement3D.cpp",
                 "refine mesh",
                 "Only works in 3D");
  }

  // Ensure connectivity is there etc
  mesh.init(1);
  mesh.init(1, tdim);

  // Instantiate a class to hold most of the refinement information
  ParallelRefinement p(mesh);
  
  // Mark all edges, and create new vertices
  p.mark_all();
  p.create_new_vertices();
  std::map<std::size_t, std::size_t>& edge_to_new_vertex = p.edge_to_new_vertex();
  
  // Generate new topology

  for(CellIterator cell(mesh); !cell.end(); ++cell)
  {
    EdgeIterator e(*cell);
    VertexIterator v(*cell);

    const std::size_t v0 = v[0].global_index();
    const std::size_t v1 = v[1].global_index();
    const std::size_t v2 = v[2].global_index();
    const std::size_t v3 = v[3].global_index();
    const std::size_t e0 = edge_to_new_vertex[e[0].index()];
    const std::size_t e1 = edge_to_new_vertex[e[1].index()];
    const std::size_t e2 = edge_to_new_vertex[e[2].index()];
    const std::size_t e3 = edge_to_new_vertex[e[3].index()];
    const std::size_t e4 = edge_to_new_vertex[e[4].index()];
    const std::size_t e5 = edge_to_new_vertex[e[5].index()];

    // Mostly duplicated from TetrahedronCell.cpp

    p.new_cell(v0, e3, e4, e5);
    p.new_cell(v1, e1, e2, e5);
    p.new_cell(v2, e0, e2, e4);
    p.new_cell(v3, e0, e1, e3);

    const Point p0 = e[0].midpoint();
    const Point p1 = e[1].midpoint();
    const Point p2 = e[2].midpoint();
    const Point p3 = e[3].midpoint();
    const Point p4 = e[4].midpoint();
    const Point p5 = e[5].midpoint();
    const double d05 = p0.distance(p5);
    const double d14 = p1.distance(p4);
    const double d23 = p2.distance(p3);

    // Then divide the remaining octahedron into 4 tetrahedra
    if (d05 <= d14 && d14 <= d23)
    {
      p.new_cell(e0, e1, e2, e5);
      p.new_cell(e0, e1, e3, e5);
      p.new_cell(e0, e2, e4, e5);
      p.new_cell(e0, e3, e4, e5);
    }
    else if (d14 <= d23)
    {
      p.new_cell(e0, e1, e2, e4);
      p.new_cell(e0, e1, e3, e4);
      p.new_cell(e1, e2, e4, e5);
      p.new_cell(e1, e3, e4, e5);
    }
    else
    {
      p.new_cell(e0, e1, e2, e3);
      p.new_cell(e0, e2, e3, e4);
      p.new_cell(e1, e2, e3, e5);
      p.new_cell(e2, e3, e4, e5);
    }

  }

  p.partition(new_mesh);

}
//-----------------------------------------------------------------------------
