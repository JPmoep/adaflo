// --------------------------------------------------------------------------
//
// Copyright (C) 2021 by the adaflo authors
//
// This file is part of the adaflo library.
//
// The adaflo library is free software; you can use it, redistribute it,
// and/or modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation; either version 2.1 of the
// License, or (at your option) any later version.  The full text of the
// license can be found in the file LICENSE at the top level of the adaflo
// distribution.
//
// --------------------------------------------------------------------------

#ifndef __adaflo_block_sharp_inteface_h
#define __adaflo_block_sharp_inteface_h

#include <deal.II/dofs/dof_renumbering.h>

#include <deal.II/fe/fe_q_iso_q1.h>
#include <deal.II/fe/mapping_fe_field.h>

#include <deal.II/grid/grid_tools_cache.h>
#include <deal.II/grid/tria.h>
#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/grid_out.h>

#include <deal.II/matrix_free/fe_point_evaluation.h>

#include <deal.II/numerics/vector_tools.h>

#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/polygon.hpp>

#include <adaflo/level_set_okz_advance_concentration.h>
#include <adaflo/level_set_okz_compute_curvature.h>
#include <adaflo/level_set_okz_compute_normal.h>
#include <adaflo/level_set_okz_preconditioner.h>
#include <adaflo/level_set_okz_reinitialization.h>
#include <adaflo/sharp_interface_util.h>
#include <adaflo/util.h>

#include <filesystem>
#include <iostream>
#include <fstream>
#include <cmath>

template <int dim>
class LevelSetSolver
{
public:
  using VectorType      = LinearAlgebra::distributed::Vector<double>;
  using BlockVectorType = LinearAlgebra::distributed::BlockVector<double>;
  ConditionalOStream    pcout;
  double                                 minimal_edge_length;
  
  static const unsigned int dof_index_ls        = 1;
  static const unsigned int dof_index_normal    = 2;
  static const unsigned int dof_index_curvature = 3;
  static const unsigned int dof_index_velocity  = 0;
  static const unsigned int quad_index          = 0;
  static const unsigned int quad_index_vel      = 1;

  LevelSetSolver(
    const Triangulation<dim> &tria,
    const Function<dim> &     initial_values_ls,
    const FlowParameters &    parameters,
    const TimeStepping &      time_stepping,
    VectorType &              velocity_solution,
    VectorType &              velocity_solution_old,
    VectorType &              velocity_solution_old_old,
    const std::map<types::boundary_id, std::shared_ptr<Function<dim>>> &fluid_type,
    const std::set<types::boundary_id> &                                symmetry)
    : pcout(std::cout, Utilities::MPI::this_mpi_process(MPI_COMM_WORLD) == 0)
    , parameters(parameters)
    , time_stepping(time_stepping)
    , last_concentration_range(-1, +1)
    , first_reinit_step(true)
    , dof_handler(tria)
    , dof_handler_dim(tria)
    , velocity_solution(velocity_solution)
    , velocity_solution_old(velocity_solution_old)
    , velocity_solution_old_old(velocity_solution_old_old)
    , normal_vector_field(dim)
    , normal_vector_rhs(dim)
  {
    {
      LevelSetOKZSolverComputeNormalParameter params;
      params.dof_index_ls            = dof_index_ls;
      params.dof_index_normal        = dof_index_normal;
      params.quad_index              = quad_index;
      params.epsilon                 = this->parameters.epsilon;
      params.approximate_projections = this->parameters.approximate_projections;

      normal_operator =
        std::make_unique<LevelSetOKZSolverComputeNormal<dim>>(normal_vector_field,
                                                              normal_vector_rhs,
                                                              ls_solution,
                                                              cell_diameters,
                                                              epsilon_used,
                                                              minimal_edge_length,
                                                              constraints_normals,
                                                              params,
                                                              matrix_free,
                                                              preconditioner,
                                                              projection_matrix,
                                                              ilu_projection_matrix);
    }

    {
      LevelSetOKZSolverReinitializationParameter params;
      params.dof_index_ls     = dof_index_ls;
      params.dof_index_normal = dof_index_normal;
      params.quad_index       = quad_index;
      params.do_iteration     = this->parameters.do_iteration;

      params.time.time_step_scheme     = this->parameters.time_step_scheme;
      params.time.start_time           = this->parameters.start_time;
      params.time.end_time             = this->parameters.end_time;
      params.time.time_step_size_start = this->parameters.time_step_size_start;
      params.time.time_stepping_cfl    = this->parameters.time_stepping_cfl;
      params.time.time_stepping_coef2  = this->parameters.time_stepping_coef2;
      params.time.time_step_tolerance  = this->parameters.time_step_tolerance;
      params.time.time_step_size_max   = this->parameters.time_step_size_max;
      params.time.time_step_size_min   = this->parameters.time_step_size_min;

      reinit =
        std::make_unique<LevelSetOKZSolverReinitialization<dim>>(normal_vector_field,
                                                                 cell_diameters,
                                                                 epsilon_used,
                                                                 minimal_edge_length,
                                                                 constraints,
                                                                 ls_update,
                                                                 ls_solution,
                                                                 ls_rhs,
                                                                 pcout,
                                                                 preconditioner,
                                                                 last_concentration_range,
                                                                 params,
                                                                 first_reinit_step,
                                                                 matrix_free);
    }

    {
      LevelSetOKZSolverComputeCurvatureParameter params;
      params.dof_index_ls            = dof_index_ls;
      params.dof_index_curvature     = dof_index_curvature;
      params.dof_index_normal        = dof_index_normal;
      params.quad_index              = quad_index;
      params.epsilon                 = this->parameters.epsilon;
      params.approximate_projections = this->parameters.approximate_projections;
      params.curvature_correction    = this->parameters.curvature_correction;

      curvature_operator =
        std::make_unique<LevelSetOKZSolverComputeCurvature<dim>>(cell_diameters,
                                                                 normal_vector_field,
                                                                 constraints_curvature,
                                                                 constraints,
                                                                 epsilon_used,
                                                                 curvature_rhs,
                                                                 params,
                                                                 curvature_solution,
                                                                 ls_solution,
                                                                 matrix_free,
                                                                 preconditioner,
                                                                 projection_matrix,
                                                                 ilu_projection_matrix);
    }

    {
      LevelSetOKZSolverAdvanceConcentrationParameter params;
      params.dof_index_ls             = dof_index_ls;
      params.dof_index_vel            = dof_index_velocity;
      params.quad_index               = quad_index;
      params.convection_stabilization = this->parameters.convection_stabilization;
      params.do_iteration             = this->parameters.do_iteration;
      params.tol_nl_iteration         = this->parameters.tol_nl_iteration;

      LevelSetOKZSolverAdvanceConcentrationBoundaryDescriptor<dim> bcs;

      bcs.dirichlet = fluid_type;
      bcs.symmetry  = symmetry;

      params.time.time_step_scheme     = this->parameters.time_step_scheme;
      params.time.start_time           = this->parameters.start_time;
      params.time.end_time             = this->parameters.end_time;
      params.time.time_step_size_start = this->parameters.time_step_size_start;
      params.time.time_stepping_cfl    = this->parameters.time_stepping_cfl;
      params.time.time_stepping_coef2  = this->parameters.time_stepping_coef2;
      params.time.time_step_tolerance  = this->parameters.time_step_tolerance;
      params.time.time_step_size_max   = this->parameters.time_step_size_max;
      params.time.time_step_size_min   = this->parameters.time_step_size_min;

      this->advection_operator =
        std::make_unique<LevelSetOKZSolverAdvanceConcentration<dim>>(
          ls_solution,
          ls_solution_old,
          ls_solution_old_old,
          ls_update,
          ls_rhs,
          velocity_solution,
          velocity_solution_old,
          velocity_solution_old_old,
          cell_diameters,
          this->constraints,
          this->pcout,
          bcs,
          this->matrix_free,
          params,
          this->preconditioner);
    }

    // MatrixFree
    {
      const FE_Q_iso_Q1<dim> fe(parameters.concentration_subdivisions);
      dof_handler.distribute_dofs(fe);

      pcout << "Number of level set degrees of freedom: " << dof_handler.n_dofs()
            << "    fe degree of level set: " << fe.degree << std::endl;

      typename MatrixFree<dim>::AdditionalData data;

      data.tasks_parallel_scheme =
        Utilities::MPI::n_mpi_processes(get_communicator(dof_handler)) > 1 ?
          MatrixFree<dim>::AdditionalData::none :
          MatrixFree<dim>::AdditionalData::partition_color;
      if (parameters.velocity_degree == 2)
        data.tasks_block_size = 16;
      else
        data.tasks_block_size = 2;
      data.store_plain_indices = true;

      const FESystem<dim> fe_dim(FE_Q<dim>(parameters.velocity_degree), dim);
      dof_handler_dim.distribute_dofs(fe_dim);

      if (parameters.precondition_velocity == FlowParameters::u_ilu)
        DoFRenumbering::Cuthill_McKee(dof_handler_dim, false, false);

      const QIterated<dim> quad(QGauss<1>(2), fe.degree);
      const QGauss<dim>    quad_vel(parameters.velocity_degree + 1);

      for (const auto &i : fluid_type)
        VectorTools::interpolate_boundary_values(mapping,
                                                 dof_handler,
                                                 i.first,
                                                 Functions::ConstantFunction<dim>(0.0),
                                                 constraints);

      constraints.close();
      constraints_curvature.close();
      constraints_normals.close();
      constraints_force.close();
      hanging_node_constraints.close();

      const std::vector<const DoFHandler<dim> *> dof_handlers{&dof_handler_dim,
                                                              &dof_handler,
                                                              &dof_handler,
                                                              &dof_handler};

      const std::vector<const AffineConstraints<double> *> all_constraints{
        &constraints_force, &constraints, &constraints_normals, &constraints_curvature};

      const std::vector<Quadrature<dim>> quadratures{quad, quad_vel};

      matrix_free.reinit(mapping, dof_handlers, all_constraints, quadratures, data);
    }

    // Vectors
    {
      initialize_dof_vector(ls_solution, dof_index_ls);
      initialize_dof_vector(ls_solution_old, dof_index_ls);
      initialize_dof_vector(ls_solution_old_old, dof_index_ls);
      initialize_dof_vector(ls_update, dof_index_ls);
      initialize_dof_vector(ls_rhs, dof_index_ls);
      initialize_dof_vector(curvature_solution, dof_index_curvature);
      initialize_dof_vector(curvature_rhs, dof_index_curvature);

      for (unsigned int i = 0; i < dim; ++i)
        {
          initialize_dof_vector(normal_vector_field.block(i), dof_index_normal);
          initialize_dof_vector(normal_vector_rhs.block(i), dof_index_normal);
        }
    }

    // miscellaneous
    {
      compute_cell_diameters(
        matrix_free, dof_index_ls, cell_diameters, minimal_edge_length, epsilon_used);

      pcout << "Mesh size (largest/smallest element length at finest level): "
            << epsilon_used << " / " << minimal_edge_length << std::endl;

      epsilon_used =
        parameters.epsilon / parameters.concentration_subdivisions * epsilon_used;

      pcout << "epsilon_used: " << epsilon_used
            << " epsilon_input: " << parameters.epsilon << std::endl;

      initialize_mass_matrix_diagonal(
        matrix_free, hanging_node_constraints, dof_index_ls, quad_index, preconditioner);

      projection_matrix     = std::make_shared<BlockMatrixExtension>();
      ilu_projection_matrix = std::make_shared<BlockILUExtension>();

      initialize_projection_matrix(matrix_free,
                                   constraints_normals,
                                   dof_index_ls,
                                   quad_index,
                                   epsilon_used,
                                   this->parameters.epsilon,
                                   cell_diameters,
                                   *projection_matrix,
                                   *ilu_projection_matrix);
    }

    VectorTools::interpolate(mapping, dof_handler, initial_values_ls, ls_solution);


    // transform_distance_function
    for (unsigned int i = 0; i < ls_solution.locally_owned_size(); i++)
      ls_solution.local_element(i) =
        -std::tanh(ls_solution.local_element(i) / (2. * epsilon_used));

    reinitialize(true);

    velocity_solution_old     = velocity_solution;
    velocity_solution_old_old = velocity_solution;
  }

  void
  initialize_dof_vector(VectorType &vec, const unsigned int dof_index)
  {
    matrix_free.initialize_dof_vector(vec, dof_index);
  }

  void
  solve()
  {
    const double step_size     = time_stepping.step_size();
    const double step_size_old = time_stepping.old_step_size();
    ls_update                  = ls_solution;

    if (step_size_old > 0)
      // U = sadd(a,b,V)=a*U+b*V
      ls_update.sadd((step_size + step_size_old) / step_size_old,
                     -step_size / step_size_old,
                     ls_solution_old);

    ls_solution_old_old = ls_solution_old;
    ls_solution_old     = ls_solution;
    ls_solution         = ls_update;

    ls_solution.update_ghost_values();
    ls_solution_old.update_ghost_values();
    ls_solution_old_old.update_ghost_values();

    this->advance_concentration();
    this->reinitialize();
  }

  const MappingQ1<dim> &
  get_mapping()
  {
    return mapping;
  }

  const FiniteElement<dim> &
  get_fe_ls() 
  {
    return this->dof_handler.get_fe();
  }


  const VectorType &
  get_level_set_vector()
  {
    return ls_solution;
  }

  const BlockVectorType &
  get_normal_vector()
  {
    return normal_vector_field;
  }

  const VectorType &
  get_curvature_vector()
  {
    return curvature_solution;
  }

  const double  &
   get_min_edge_length()
   {
     return minimal_edge_length;
   }    

   const double  &
   get_eps_used()
   {
     return epsilon_used;
   }     

  const DoFHandler<dim> &
  get_dof_handler() const
  {
    return dof_handler;
  }

  const DoFHandler<dim> &
  get_dof_handler_dim() const
  {
    return dof_handler_dim;
  }

  MatrixFree<dim, double> &
  get_matrix_free()
  {
    return matrix_free;
  }

private:
  void
  advance_concentration()
  {
    velocity_solution.update_ghost_values();
    velocity_solution_old.update_ghost_values();
    velocity_solution_old_old.update_ghost_values();
    advection_operator->advance_concentration(this->time_stepping.step_size());
    velocity_solution.zero_out_ghost_values();
    velocity_solution_old.zero_out_ghost_values();
    velocity_solution_old_old.zero_out_ghost_values();
  }

  void
  reinitialize(const bool initialization = false)
  {
    const double       dt         = this->time_stepping.step_size();
    const unsigned int stab_steps = initialization ?
                                      this->parameters.n_initial_reinit_steps :
                                      this->parameters.n_reinit_steps;
    const unsigned int diff_steps = 0;
    reinit->reinitialize(dt, stab_steps, diff_steps, [this](const bool fast) {
      normal_operator->compute_normal(fast);
    });

    normal_operator->compute_normal(/*fast_computation*/ false);

    curvature_operator->compute_curvature(/*diffuse_large_values*/ false);
  }

  //ConditionalOStream    pcout;
  const FlowParameters &parameters;
  const TimeStepping &  time_stepping;


  std::pair<double, double>              last_concentration_range;
  bool                                   first_reinit_step;
  AlignedVector<VectorizedArray<double>> cell_diameters;
  //double                                 minimal_edge_length;
  double                                 epsilon_used;

  MappingQ1<dim> mapping;
  //const FESystem<dim> fe;

  // DoFHandlers
  DoFHandler<dim> dof_handler;     // for ls, normal, curvature
  DoFHandler<dim> dof_handler_dim; // for velocity field

  // Constraints
  AffineConstraints<double> constraints;
  AffineConstraints<double> constraints_normals;
  AffineConstraints<double> hanging_node_constraints;
  AffineConstraints<double> constraints_curvature;
  AffineConstraints<double> constraints_force;

  // MatrixFree
  MatrixFree<dim, double> matrix_free;

  // vectors: velocity (external)
  VectorType &velocity_solution;
  VectorType &velocity_solution_old;
  VectorType &velocity_solution_old_old;

  // ... level set
  VectorType ls_solution;
  VectorType ls_solution_old;
  VectorType ls_solution_old_old;
  VectorType ls_update;
  VectorType ls_rhs;

  // ... normal
  BlockVectorType normal_vector_field;
  BlockVectorType normal_vector_rhs;

  // ... curvature
  VectorType curvature_solution;
  VectorType curvature_rhs;

  // Preconditioners
  DiagonalPreconditioner<double>        preconditioner;
  std::shared_ptr<BlockMatrixExtension> projection_matrix;
  std::shared_ptr<BlockILUExtension>    ilu_projection_matrix;

  // Operators
  std::unique_ptr<LevelSetOKZSolverComputeNormal<dim>>        normal_operator;
  std::unique_ptr<LevelSetOKZSolverReinitialization<dim>>     reinit;
  std::unique_ptr<LevelSetOKZSolverComputeCurvature<dim>>     curvature_operator;
  std::unique_ptr<LevelSetOKZSolverAdvanceConcentration<dim>> advection_operator;
};

//template class LevelSetSolver<2>;


class SharpInterfaceSolver
{
public:
  virtual void
  advance_time_step() = 0;

  virtual void
  output_solution(const std::string &output_filename) = 0;

  std::pair<double, double>
  get_concentration_range() ;

  std::vector<Point<2>> 
  get_level_set_interface();

  virtual std::vector<double>
  compute_bubble_statistics(
    const double                global_omega_diameter,
    const unsigned int           sub_refinements  = numbers::invalid_unsigned_int) = 0;

};



template <int dim>
class FrontTrackingSolver : public SharpInterfaceSolver
{
public:
  using VectorType = LinearAlgebra::distributed::Vector<double>;

  FrontTrackingSolver(NavierStokes<dim> &          navier_stokes_solver,
                      Triangulation<dim - 1, dim> &surface_mesh)
    : navier_stokes_solver(navier_stokes_solver)
    , surface_dofhandler_dim(surface_mesh)
    , surface_dofhandler(surface_mesh)
  {
    // Degree for FE at surface mesh
    const unsigned int fe_degree      = 3;
    const unsigned int mapping_degree = fe_degree;

    FESystem<dim - 1, dim> euler_fe(FE_Q<dim - 1, dim>(fe_degree), dim);
    surface_dofhandler_dim.distribute_dofs(euler_fe);

    surface_dofhandler.distribute_dofs(FE_Q<dim - 1, dim>(fe_degree));

    surface_coordinates_vector.reinit(surface_dofhandler_dim.n_dofs());
    surface_coordinates_vector.update_ghost_values();
    VectorTools::get_position_vector(MappingQGeneric<dim - 1, dim>(mapping_degree),
                                     surface_dofhandler_dim,
                                     surface_coordinates_vector);
    surface_coordinates_vector.zero_out_ghost_values();

    euler_mapping = std::make_shared<MappingFEField<dim - 1, dim, VectorType>>(
      surface_dofhandler_dim, surface_coordinates_vector);


    this->update_phases();
    this->update_gravity_force();
    this->update_surface_tension();
  }

  void
  advance_time_step() override
  {
    this->move_surface_mesh();
    this->update_phases();
    this->update_gravity_force();
    this->update_surface_tension();

    navier_stokes_solver.get_constraints_u().set_zero(
      navier_stokes_solver.user_rhs.block(0));
    navier_stokes_solver.advance_time_step();
  }

  void
  output_solution(const std::string &output_filename) override
  {
    // background mesh
    {
      DataOutBase::VtkFlags flags;
      flags.write_higher_order_cells = true;

      DataOut<dim> data_out;
      data_out.set_flags(flags);

      std::vector<DataComponentInterpretation::DataComponentInterpretation>
        vector_component_interpretation(
          dim, DataComponentInterpretation::component_is_part_of_vector);

      navier_stokes_solver.solution.update_ghost_values();

      data_out.add_data_vector(navier_stokes_solver.get_dof_handler_u(),
                               navier_stokes_solver.solution.block(0),
                               std::vector<std::string>(dim, "velocity"),
                               vector_component_interpretation);

      data_out.add_data_vector(navier_stokes_solver.get_dof_handler_u(),
                               navier_stokes_solver.user_rhs.block(0),
                               std::vector<std::string>(dim, "user_rhs"),
                               vector_component_interpretation);

      data_out.add_data_vector(navier_stokes_solver.get_dof_handler_p(),
                               navier_stokes_solver.solution.block(1),
                               "pressure");

      data_out.build_patches(navier_stokes_solver.mapping,
                             navier_stokes_solver.get_dof_handler_u().get_fe().degree +
                               1);

      navier_stokes_solver.write_data_output(
        output_filename,
        navier_stokes_solver.time_stepping,
        navier_stokes_solver.get_parameters().output_frequency,
        navier_stokes_solver.get_dof_handler_u().get_triangulation(),
        data_out);
    }

    // surface mesh
    {
      DataOutBase::VtkFlags flags;

      DataOut<dim - 1, dim> data_out;
      data_out.set_flags(flags);
      data_out.add_data_vector(surface_dofhandler, curvature_vector, "curvature");
      data_out.add_data_vector(surface_dofhandler_dim, normal_vector, "normal");

      data_out.build_patches(*euler_mapping,
                             surface_dofhandler_dim.get_fe().degree + 1,
                             DataOut<dim - 1, dim>::CurvedCellRegion::curved_inner_cells);

      std::filesystem::path path(output_filename + "_surface");

      data_out.write_vtu_with_pvtu_record(path.parent_path().string() + "/",
                                          path.filename(),
                                          navier_stokes_solver.time_stepping.step_no(),
                                          MPI_COMM_WORLD);
    }
  }

  //TODO: fill function if needed
  std::pair<double, double>
  get_concentration_range() 
  {
    return concentration;
  }

  //TODO fill function if needed yesterday
  std::vector<Point<dim>> 
  get_level_set_interface() 
  {
    return interface_points_mc;
  }

  //TODO: fill function if needed
  std::vector<double> 
  compute_bubble_statistics(
    const double               global_omega_diameter,
    const unsigned int         sub_refinements) override
  {
    std::vector<double> data(4 + 2 * dim);
    return data;
  }


private:
  void
  move_surface_mesh()
  {
    VectorTools::update_position_vector(navier_stokes_solver.time_stepping.step_size(),
                                        navier_stokes_solver.get_dof_handler_u(),
                                        navier_stokes_solver.mapping,
                                        navier_stokes_solver.solution.block(0),
                                        surface_dofhandler_dim,
                                        *euler_mapping,
                                        surface_coordinates_vector);

  }

  void
  update_phases()
  {
    const auto density        = navier_stokes_solver.get_parameters().density;
    const auto density_diff   = navier_stokes_solver.get_parameters().density_diff;
    const auto viscosity      = navier_stokes_solver.get_parameters().viscosity;
    const auto viscosity_diff = navier_stokes_solver.get_parameters().viscosity_diff;

    if (density_diff == 0.0 && viscosity_diff == 0.0)
      return; // nothing to do

    boost::geometry::model::polygon<boost::geometry::model::d2::point_xy<double>> polygon;
    GridTools::construct_polygon(*euler_mapping, surface_dofhandler_dim, polygon);

    double dummy;

    navier_stokes_solver.matrix_free->template cell_loop<double, double>(
      [&](const auto &matrix_free, auto &, const auto &, auto macro_cells) {
        FEEvaluation<dim, -1, 0, 1, double> phi(matrix_free, 0, 0); 

        for (unsigned int cell = macro_cells.first; cell < macro_cells.second; ++cell)
          {
            phi.reinit(cell);

            for (unsigned int q = 0; q < phi.n_q_points; ++q)
              {
                const auto indicator =
                  GridTools::within(polygon, phi.quadrature_point(q));

                navier_stokes_solver.get_matrix().begin_densities(cell)[q] =
                  density + density_diff * indicator;
                navier_stokes_solver.get_matrix().begin_viscosities(cell)[q] =
                  viscosity + viscosity_diff * indicator;
              }
          }
      },
      dummy,
      dummy);
  }

  void
  update_surface_tension()
  {
    // return; // TODO: not working

    normal_vector.reinit(surface_dofhandler_dim.n_dofs());
    curvature_vector.reinit(surface_dofhandler.n_dofs());

    compute_normal(*euler_mapping, surface_dofhandler_dim, normal_vector);
    compute_curvature(*euler_mapping,
                      surface_dofhandler_dim,
                      surface_dofhandler,
                      QGaussLobatto<dim - 1>(surface_dofhandler.get_fe().degree + 1),
                      normal_vector,
                      curvature_vector);

    compute_force_vector_sharp_interface(
      *euler_mapping,
      surface_dofhandler,
      surface_dofhandler_dim,
      QGauss<dim - 1>(surface_dofhandler_dim.get_fe().degree + 1),
      navier_stokes_solver.mapping,
      navier_stokes_solver.get_dof_handler_u(),
      navier_stokes_solver.get_parameters().surface_tension,
      normal_vector,
      curvature_vector,
      navier_stokes_solver.user_rhs.block(0));
  }

  void
  update_gravity_force()
  {
    const auto gravity = navier_stokes_solver.get_parameters().gravity;

    const auto density      = navier_stokes_solver.get_parameters().density;
    const auto density_diff = navier_stokes_solver.get_parameters().density_diff;

    const bool zero_out = true;

    navier_stokes_solver.matrix_free->template cell_loop<VectorType, std::nullptr_t>(
      [&](const auto &matrix_free, auto &vec, const auto &, auto macro_cells) {
        FEEvaluation<dim, -1, 0, dim, double> phi(matrix_free, 0, 0);

        for (unsigned int cell = macro_cells.first; cell < macro_cells.second; ++cell)
          {
            phi.reinit(cell);

            for (unsigned int q = 0; q < phi.n_q_points; ++q)
              {
                Tensor<1, dim, VectorizedArray<double>> force;

                force[dim - 1] -=
                  gravity *
                  (density_diff == 0.0 ?
                     VectorizedArray<double>(density) :
                     navier_stokes_solver.get_matrix().begin_densities(cell)[q]);
                phi.submit_value(force, q);
              }
            phi.integrate_scatter(true, false, vec);
          }
      },
      navier_stokes_solver.user_rhs.block(0),
      nullptr,
      zero_out);
  }
  // data for bubble statistics
  std::pair<double, double> concentration;

  // background mesh
  NavierStokes<dim> &navier_stokes_solver;

  // surface mesh
  DoFHandler<dim - 1, dim>               surface_dofhandler_dim;
  DoFHandler<dim - 1, dim>               surface_dofhandler;
  VectorType                             surface_coordinates_vector;
  std::shared_ptr<Mapping<dim - 1, dim>> euler_mapping;

  std::vector<Point<dim>>     interface_points_mc;
  VectorType normal_vector;
  VectorType curvature_vector;
};



template <int dim>
class MixedLevelSetSolver : public SharpInterfaceSolver
{
public:
  using VectorType = LinearAlgebra::distributed::Vector<double>;

  MixedLevelSetSolver(NavierStokes<dim> &          navier_stokes_solver,
                      Triangulation<dim - 1, dim> &surface_mesh,
                      const Function<dim> &        initial_values_ls)
    : use_auxiliary_surface_mesh(true)
    , use_sharp_interface(true)
    , navier_stokes_solver(navier_stokes_solver)
    , level_set_solver(navier_stokes_solver.get_dof_handler_u().get_triangulation(),
                       initial_values_ls,
                       navier_stokes_solver.get_parameters(),
                       navier_stokes_solver.time_stepping,
                       navier_stokes_solver.solution.block(0),
                       navier_stokes_solver.solution_old.block(0),
                       navier_stokes_solver.solution_old_old.block(0),
                       navier_stokes_solver.boundary->fluid_type,
                       navier_stokes_solver.boundary->symmetry)
    , euler_dofhandler(surface_mesh)
  {
    // Degree for FE at surface mesh
    const unsigned int fe_degree = 1;

    FESystem<dim - 1, dim> surface_fe_dim(FE_Q<dim - 1, dim>(fe_degree), dim);
    euler_dofhandler.distribute_dofs(surface_fe_dim);

    euler_vector.reinit(euler_dofhandler.n_dofs());

    euler_vector.update_ghost_values();

    VectorTools::
      get_position_vector(MappingQGeneric<dim - 1, dim>(4 /*TODO: this is a high number to well represent curved surfaces, the actual value is not that relevant*/), euler_dofhandler, euler_vector);
    euler_vector.zero_out_ghost_values();
    euler_mapping =
      std::make_shared<MappingFEField<dim - 1, dim, VectorType>>(euler_dofhandler,
                                                                 euler_vector);

    levelset_at_surface_vector.reinit(euler_dofhandler.n_dofs());

    // initialize
    this->update_phases();
    this->update_gravity_force();
    this->update_surface_tension();
  }

  MixedLevelSetSolver(NavierStokes<dim> &         navier_stokes_solver,
                      const Function<dim>         &initial_values_ls,
                      const bool           use_sharp_interface = true)
    : use_auxiliary_surface_mesh(false)
    , use_sharp_interface(use_sharp_interface)
    , navier_stokes_solver(navier_stokes_solver)
    , level_set_solver(navier_stokes_solver.get_dof_handler_u().get_triangulation(),
                       initial_values_ls,
                       navier_stokes_solver.get_parameters(),
                       navier_stokes_solver.time_stepping,
                       navier_stokes_solver.solution.block(0),
                       navier_stokes_solver.solution_old.block(0),
                       navier_stokes_solver.solution_old_old.block(0),
                       navier_stokes_solver.boundary->fluid_type,
                       navier_stokes_solver.boundary->symmetry)
  {
    // initialize
    this->update_phases();
    this->update_gravity_force();
    this->update_surface_tension();
  }

  void
  advance_time_step() override
  {
    level_set_solver.solve();

    if (use_auxiliary_surface_mesh)
      this->move_surface_mesh();
    this->update_phases();
    this->update_gravity_force();
    this->update_surface_tension();

    navier_stokes_solver.get_constraints_u().set_zero(
      navier_stokes_solver.user_rhs.block(0));
    navier_stokes_solver.advance_time_step();
  }

  void
  output_solution(const std::string &output_filename) override
  {
    // background mesh
    {
      DataOutBase::VtkFlags flags;
      flags.write_higher_order_cells = true;

      DataOut<dim> data_out;
      data_out.set_flags(flags);

      std::vector<DataComponentInterpretation::DataComponentInterpretation>
        vector_component_interpretation(
          dim, DataComponentInterpretation::component_is_part_of_vector);

      navier_stokes_solver.solution.update_ghost_values();
      navier_stokes_solver.user_rhs.update_ghost_values();
      level_set_solver.get_level_set_vector().update_ghost_values();
      level_set_solver.get_curvature_vector().update_ghost_values();
      level_set_solver.get_normal_vector().update_ghost_values();

      data_out.add_data_vector(navier_stokes_solver.get_dof_handler_u(),
                               navier_stokes_solver.solution.block(0),
                               std::vector<std::string>(dim, "velocity"),
                               vector_component_interpretation);

      data_out.add_data_vector(navier_stokes_solver.get_dof_handler_u(),
                               navier_stokes_solver.user_rhs.block(0),
                               std::vector<std::string>(dim, "user_rhs"),
                               vector_component_interpretation);

      data_out.add_data_vector(navier_stokes_solver.get_dof_handler_p(),
                               navier_stokes_solver.solution.block(1),
                               "pressure");

      data_out.add_data_vector(level_set_solver.get_dof_handler(),
                               level_set_solver.get_level_set_vector(),
                               "level_set");

      data_out.add_data_vector(level_set_solver.get_dof_handler(),
                               level_set_solver.get_curvature_vector(),
                               "curvature");

      for (unsigned int i = 0; i < dim; ++i)
        data_out.add_data_vector(level_set_solver.get_dof_handler(),
                                 level_set_solver.get_normal_vector().block(i),
                                 "normal_" + std::to_string(i));

      data_out.build_patches(navier_stokes_solver.mapping,
                             navier_stokes_solver.get_dof_handler_u().get_fe().degree +
                               1);

      navier_stokes_solver.solution.zero_out_ghost_values();
      navier_stokes_solver.user_rhs.zero_out_ghost_values();
      level_set_solver.get_level_set_vector().zero_out_ghost_values();
      level_set_solver.get_curvature_vector().zero_out_ghost_values();
      level_set_solver.get_normal_vector().zero_out_ghost_values();

      navier_stokes_solver.write_data_output(
        output_filename,
        navier_stokes_solver.time_stepping,
        navier_stokes_solver.get_parameters().output_frequency,
        navier_stokes_solver.get_dof_handler_u().get_triangulation(),
        data_out);
    }

    // surface mesh
    if (use_auxiliary_surface_mesh)
      {
        DataOutBase::VtkFlags flags;

        DataOut<dim - 1, dim> data_out;
        data_out.set_flags(flags);
        data_out.attach_dof_handler(euler_dofhandler);
        data_out.add_data_vector(euler_dofhandler, levelset_at_surface_vector, "levelset");

        data_out.build_patches(
          *euler_mapping,
          euler_dofhandler.get_fe().degree + 1,
          DataOut<dim - 1, dim>::CurvedCellRegion::curved_inner_cells);

        std::filesystem::path path(output_filename + "_surface");

        data_out.write_vtu_with_pvtu_record(path.parent_path().string() + "/",
                                            path.filename(),
                                            navier_stokes_solver.time_stepping.step_no(),
                                            MPI_COMM_WORLD);


      // interface (Phi=0) contour triangulation
      //TODO: file empty! change file format, output in folder
      /*std::ofstream out("contour_grid.vtk");
      GridOut       grid_out;
      grid_out.write_vtk(contour_tria, out);
      std::cout << "Grid written to contour_grid.vtk" << std::endl;
      */

      bool first_output = true;
      std::ostringstream filename_contour;
        filename_contour << output_filename << "_contour"
                  << "-" << navier_stokes_solver.time_stepping.step_no() << ".csv";

        std::fstream output_positions(filename_contour.str().c_str(),
                                        first_output ? std::ios::out :
                                                      std::ios::out | std::ios::app);

        output_positions.precision(14);
        if (first_output)
          output_positions
            << "x_pos, y_pos, z_pos"
            << std::endl;
        for (unsigned int i = 0; i < interface_points_mc.size(); ++i)
          {
            //output_positions3 << "  ";
            for (unsigned int j = 0; j < dim; ++j)
              output_positions << interface_points_mc[i][j] << ",";
            output_positions << std::endl;
          }
          //std::cout << "size interface pts = " << interface_points_mc.size() << " dim = " << dim << std::endl;
        interface_points_mc.clear();
        first_output = false;
        std::cout << "Grid written to contour.csv" << std::endl;
      }
  }

  // TODO: check if working at this postition???

  //template <>
  std::pair<double, double>
  get_concentration_range() 
  {
    //const unsigned int dim = 2;
    const QIterated<dim> quadrature_formula(QTrapez<1>(), level_set_solver.get_fe_ls().degree + 2);
    FEValues<dim>        fe_values(level_set_solver.get_fe_ls(), quadrature_formula, update_values);
    const unsigned int   n_q_points = quadrature_formula.size();
    std::vector<double>  concentration_values(n_q_points);
    Vector<double>          sol_values(level_set_solver.get_fe_ls().dofs_per_cell);

    double min_concentration = std::numeric_limits<double>::max(),
          max_concentration = -min_concentration;
      
    /*typename DoFHandler<dim>::active_cell_iterator cell = level_set_solver.get_dof_handler().begin_active(),
                                                  endc = level_set_solver.get_dof_handler().end();
    for (; cell != endc; ++cell) */
    for (const auto &cell : level_set_solver.get_dof_handler().active_cell_iterators())
      if (cell->is_locally_owned())
        {
          fe_values.reinit(cell);
          fe_values.get_function_values(level_set_solver.get_level_set_vector(), concentration_values);
          for (unsigned int q = 0; q < n_q_points; ++q)
            {
              const double concentration = concentration_values[q];
              
              min_concentration = std::min(min_concentration, concentration);
              max_concentration = std::max(max_concentration, concentration);
            }
        }
    last_concentration_range = std::make_pair(
      -Utilities::MPI::max(-min_concentration, get_communicator(navier_stokes_solver.get_dof_handler_u().get_triangulation())),
      Utilities::MPI::max(max_concentration, get_communicator(navier_stokes_solver.get_dof_handler_u().get_triangulation())));
    return last_concentration_range;
  }

  std::vector<double> 
  compute_bubble_statistics(
    const double               global_omega_diameter,
    const unsigned int         sub_refinements) override
  {
    //const unsigned int dim = 2;

    const int sub_per_d = sub_refinements == numbers::invalid_unsigned_int ?
                            navier_stokes_solver.get_parameters().velocity_degree + 3 :
                            sub_refinements;
    const QIterated<dim> quadrature_formula(QTrapez<1>(), sub_per_d);
    const QGauss<dim>    interior_quadrature(navier_stokes_solver.get_parameters().velocity_degree);
    const unsigned int   n_q_points = quadrature_formula.size();
    FEValues<dim>        fe_values(navier_stokes_solver.mapping,
                            level_set_solver.get_dof_handler().get_fe(),
                            quadrature_formula,
                            update_values | update_JxW_values | update_quadrature_points);
    FEValues<dim>        ns_values(navier_stokes_solver.mapping,
                            navier_stokes_solver.get_fe_u(),
                            quadrature_formula,
                            update_values);
    FEValues<dim>        interior_ns_values(navier_stokes_solver.mapping,
                                    navier_stokes_solver.get_fe_u(),
                                    interior_quadrature,
                                    update_values | update_JxW_values |
                                      update_quadrature_points);

    std::vector<Tensor<2, 2>> *interface_points = 0;
    const FEValuesExtractors::Vector vel(0);

    const unsigned int n_points       = 2 * (dim > 1 ? 2 : 1) * (dim > 2 ? 2 : 1),
                      n_subdivisions = (sub_per_d) * (dim > 1 ? (sub_per_d) : 1) *
                                        (dim > 2 ? (sub_per_d) : 1); 
    std::vector<double> full_c_values(n_q_points), c_values(n_points),
      quad_weights(n_points), weight_correction(n_q_points);
    std::vector<Tensor<1, dim>> velocity_values(n_q_points), velocities(n_points),
      int_velocity_values(interior_quadrature.size());
    std::vector<Point<dim>> quad(n_points);
    Vector<double>          sol_values(level_set_solver.get_dof_handler().get_fe().dofs_per_cell);
    std::vector<double>          ls_values(n_q_points);


    for (unsigned int i = 0; i < n_q_points; i++)
      {
        weight_correction[i] = 1;
        unsigned int fact    = sub_per_d + 1;
        if (i % fact > 0 && i % fact < fact - 1)
          weight_correction[i] *= 0.5;
        if (i >= fact && i < n_q_points - fact)
          weight_correction[i] *= 0.5;
      }

    if (interface_points != 0)
      interface_points->clear();
    double                                area = 0, perimeter = 0;
    Tensor<1, dim>                        center_of_mass, velocity;
    
    typename DoFHandler<dim>::active_cell_iterator cell = level_set_solver.get_dof_handler().begin_active(),
                                          endc = level_set_solver.get_dof_handler().end();
    
    typename DoFHandler<dim>::active_cell_iterator ns_cell =
      navier_stokes_solver.get_dof_handler_u().begin_active();
    
    for (; cell != endc; ++cell, ++ns_cell)
      if (cell->is_locally_owned())
        {
          // cheap test: find out whether the interface crosses this cell,
          // i.e. two solution values have a different sign. if not, can compute
          // with a low order Gauss quadrature without caring about the interface
          cell->get_interpolated_dof_values(level_set_solver.get_level_set_vector(), sol_values);
          bool interface_crosses_cell = false;
          for (unsigned int i = 1; i < level_set_solver.get_fe_ls().dofs_per_cell; ++i){
            if (sol_values(i) * sol_values(0) <= 0){
              interface_crosses_cell = true;
            }
          }
          if (interface_crosses_cell == false)
            {
              bool has_area = sol_values(0) > 0;
              interior_ns_values.reinit(ns_cell);
              interior_ns_values[vel].get_function_values(navier_stokes_solver.solution.block(0),
                                                          int_velocity_values);
              for (unsigned int q = 0; q < interior_quadrature.size(); q++)
                {
                  if (has_area)
                    {
                      area += interior_ns_values.JxW(q);
                      for (unsigned int d = 0; d < dim; ++d)
                        {
                          center_of_mass[d] += (interior_ns_values.quadrature_point(q)[d] *
                                                interior_ns_values.JxW(q));
                          velocity[d] +=
                            (int_velocity_values[q][d] * interior_ns_values.JxW(q));
                        }
                    }
                }
              continue;
            }

          // when the interface crosses this cell, have to find the crossing
          // points (linear interpolation) and compute the area fraction
          fe_values.reinit(cell);
          fe_values.get_function_values(level_set_solver.get_level_set_vector(), full_c_values);
          ns_values.reinit(ns_cell);
          ns_values[vel].get_function_values(navier_stokes_solver.solution.block(0),
                                            velocity_values);

          for (unsigned int d = 0; d < n_subdivisions; d++)
            {
              // compute a patch of four points
              {
                const int initial_shift = d % sub_per_d + (d / sub_per_d) * (sub_per_d + 1);
                for (unsigned int i = 0; i < n_points; i++)
                  {
                    const unsigned int index =
                      initial_shift + (i / 2) * (sub_per_d - 1) + i;
                    Assert(index < n_q_points, ExcInternalError());
                    c_values[i]     = full_c_values[index];
                    velocities[i]   = velocity_values[index];
                    quad[i]         = fe_values.quadrature_point(index);
                    quad_weights[i] = fe_values.JxW(index) * weight_correction[index];
                  }
              }
              double         local_area = 1;
              double         int_rx0 = -1, int_rx1 = -1, int_ry0 = -1, int_ry1 = -1;
              Tensor<1, dim> pos_x0, pos_x1, pos_y0, pos_y1;

              // add a small perturbation to avoid having exact zero values
              for (unsigned int i = 0; i < n_points; ++i)
                c_values[i] += 1e-22;

              // locate interface
              if (c_values[0] * c_values[1] <= 0)
                {
                  int_rx0 = c_values[0] / (c_values[0] - c_values[1]);
                  pos_x0  = quad[0] + (quad[1] - quad[0]) * int_rx0;
                }
              if (c_values[2] * c_values[3] <= 0)
                {
                  int_rx1 = c_values[2] / (c_values[2] - c_values[3]);
                  pos_x1  = quad[2] + (quad[3] - quad[2]) * int_rx1;
                }
              if (c_values[0] * c_values[2] <= 0)
                {
                  int_ry0 = c_values[0] / (c_values[0] - c_values[2]);
                  pos_y0  = quad[0] + (quad[2] - quad[0]) * int_ry0;
                }
              if (c_values[1] * c_values[3] <= 0)
                {
                  int_ry1 = c_values[1] / (c_values[1] - c_values[3]);
                  pos_y1  = quad[1] + (quad[3] - quad[1]) * int_ry1;
                }
              Tensor<1, dim> difference;
              Tensor<2, dim> interface_p;
              if (int_rx0 > 0)
                {
                  if (int_ry0 > 0)
                    {
                      const double my_area = 0.5 * int_rx0 * int_ry0;
                      local_area -= (c_values[0] < 0) ? my_area : 1 - my_area;
                      difference = pos_x0 - pos_y0;
                      perimeter += difference.norm();
                      interface_p[0] = pos_x0;
                      interface_p[1] = pos_y0;
                    }
                  if (int_ry1 > 0)
                    {
                      const double my_area = 0.5 * (1 - int_rx0) * int_ry1;
                      local_area -= (c_values[1] < 0) ? my_area : 1 - my_area;
                      difference = pos_x0 - pos_y1;
                      perimeter += difference.norm();
                      interface_p[0] = pos_x0;
                      interface_p[1] = pos_y1;
                    }
                  if (int_rx1 > 0 && int_ry0 < 0 && int_ry1 < 0)
                    {
                      const double my_area = 0.5 * (int_rx0 + int_rx1);
                      local_area -= (c_values[0] < 0) ? my_area : 1 - my_area;
                      difference = pos_x0 - pos_x1;
                      perimeter += difference.norm();
                      interface_p[0] = pos_x0;
                      interface_p[1] = pos_x1;
                    }
                }
              if (int_rx1 > 0)
                {
                  if (int_ry0 > 0)
                    {
                      const double my_area = 0.5 * int_rx1 * (1 - int_ry0);
                      local_area -= (c_values[2] < 0) ? my_area : 1 - my_area;
                      difference = pos_x1 - pos_y0;
                      perimeter += difference.norm();
                      interface_p[0] = pos_x1;
                      interface_p[1] = pos_y0;
                    }
                  if (int_ry1 > 0)
                    {
                      const double my_area = 0.5 * (1 - int_rx1) * (1 - int_ry1);
                      local_area -= (c_values[3] < 0) ? my_area : 1 - my_area;
                      difference = pos_x1 - pos_y1;
                      perimeter += difference.norm();
                      interface_p[0] = pos_x1;
                      interface_p[1] = pos_y1;
                    }
                }
              if (int_ry0 > 0 && int_ry1 > 0 && int_rx0 < 0 && int_rx1 < 0)
                {
                  const double my_area = 0.5 * (int_ry0 + int_ry1);
                  local_area -= (c_values[0] < 0) ? my_area : 1 - my_area;
                  difference = pos_y0 - pos_y1;
                  perimeter += difference.norm();
                  interface_p[0] = pos_y0;
                  interface_p[1] = pos_y1;
                }
              if (int_rx0 <= 0 && int_rx1 <= 0 && int_ry0 <= 0 && int_ry1 <= 0 &&
                  c_values[0] <= 0)
                local_area = 0;

              if (interface_p != Tensor<2, dim>() && interface_points != 0)
                interface_points->push_back(interface_p);

              Assert(local_area >= 0, ExcMessage("Substracted too much"));
              for (unsigned int i = 0; i < n_points; ++i)
                {
                  double my_area = local_area * quad_weights[i];
                  area += my_area;
                  for (unsigned int d = 0; d < dim; ++d)
                    {
                      center_of_mass[d] += quad[i][d] * my_area;
                      velocity[d] += velocities[i][d] * my_area;
                    }
                }
            }
        }

    const MPI_Comm &mpi_communicator = get_communicator(navier_stokes_solver.get_dof_handler_u().get_triangulation());

    const double global_area      = Utilities::MPI::sum(area, mpi_communicator);
    const double global_perimeter = Utilities::MPI::sum(perimeter, mpi_communicator);
    level_set_solver.pcout << "area = " << area << "   perimeter = " << perimeter << std::endl;

    Tensor<1, dim> global_mass_center;
    Tensor<1, dim> global_velocity;

    for (unsigned int d = 0; d < dim; ++d)
      {
        global_velocity[d]    = Utilities::MPI::sum(velocity[d], mpi_communicator);
        global_mass_center[d] = Utilities::MPI::sum(center_of_mass[d], mpi_communicator);
        level_set_solver.pcout << "d = " << d << " :   center of mass = " << center_of_mass[d] <<  std::endl;
      }
    
    //TODO: L2 norm of level set at surface
    double ls_max = 0.0;
    double sum = 0.0;
    for(unsigned int i= 0; i < level_set_at_surface.size(); ++i)
    {
      sum += level_set_at_surface[i]*level_set_at_surface[i];
      ls_max = std::max(level_set_at_surface[i], ls_max);
    }
    double ls_norm = std::sqrt(sum);  
    unsigned int n_iter = level_set_at_surface_max.size();
    
    this->set_adaptive_time_step(global_velocity.norm() / global_area);

    const double circularity = 2. * std::sqrt(global_area * numbers::PI) / global_perimeter;
    if (navier_stokes_solver.get_parameters().output_verbosity > 0)
      {
        const std::size_t old_precision = std::cout.precision();
        std::cout.precision(8);
        level_set_solver.pcout << "  Degree of circularity: " << circularity << std::endl;
        level_set_solver.pcout << "  Mean bubble velocity: ";
        for (unsigned int d = 0; d < dim; ++d)
          level_set_solver.pcout << ((std::abs(global_velocity[d]) < 1e-7 * global_velocity.norm()) ?
                      0. :
                      (global_velocity[d] / global_area))
                << "  ";
        level_set_solver.pcout << std::endl;
        level_set_solver.pcout << "  Position of the center of mass:  ";
        for (unsigned int d = 0; d < dim; ++d)
          level_set_solver.pcout << ((std::abs(global_mass_center[d]) < 1e-7 * global_omega_diameter) ?
                      0. :
                      (global_mass_center[d] / global_area))
                << "  ";
        level_set_solver.pcout << std::endl;
        level_set_solver.pcout << "with diameter = " << global_omega_diameter << std::endl;

       std::pair<double, double> concentration = get_concentration_range();
        level_set_solver.pcout << "  Range of level set values: " << concentration.first << " / "
              << concentration.second << std::endl;

        level_set_solver.pcout << "  Max of level set at surface: " << ls_max << "   l2 norm of ls at surface: " << ls_norm << std::endl;
  
        std::cout.precision(old_precision);
      }

    //TODO: put into data vector? level_set_at_surface_max, level_set_at_surface_norm
    std::vector<double> data(4 + 2 * dim + 2 * n_iter);
    data[0] = navier_stokes_solver.time_stepping.now();
    data[1] = global_area;
    data[2] = global_perimeter;
    data[3] = circularity;
    for (unsigned int d = 0; d < dim; ++d)
      data[4 + d] = global_velocity[d] / global_area;
    for (unsigned int d = 0; d < dim; ++d)
      data[4 + dim + d] = global_mass_center[d] / global_area;
    for (unsigned int i = 0; i < n_iter; ++i)
      data[4 + 2*dim + i] = level_set_at_surface_max[i];
    for (unsigned int i = 0; i < n_iter; ++i)
      data[4 + 2*dim + n_iter + i] = level_set_at_surface_norm[i];
    

    // get interface points from other processors
    if (interface_points != 0)
      {
        std::vector<unsigned int> receive_count(
          Utilities::MPI::n_mpi_processes(mpi_communicator));

        unsigned int n_send_elements = interface_points->size();

        MPI_Gather(&n_send_elements,
                  1,
                  MPI_UNSIGNED,
                  &receive_count[0],
                  1,
                  MPI_UNSIGNED,
                  0,
                  mpi_communicator);
        for (unsigned int i = 1; i < Utilities::MPI::n_mpi_processes(mpi_communicator); ++i)
          {
            // Each processor sends the interface_points he deals with to
            // processor
            // 0
            if (Utilities::MPI::this_mpi_process(mpi_communicator) == i)
              {
                // put data into a std::vector<double> to create a data type that
                // MPI understands
                std::vector<double> send_data(2 * dim * interface_points->size());
                for (unsigned int j = 0; j < interface_points->size(); ++j)
                  for (unsigned int d = 0; d < 2; ++d)
                    for (unsigned int e = 0; e < dim; ++e)
                      send_data[j * 2 * dim + d * dim + e] = (*interface_points)[j][d][e];
                MPI_Send(
                  &send_data[0], send_data.size(), MPI_DOUBLE, 0, i, mpi_communicator);

                // when we are done with sending, destroy the data on all
                // processors except processor 0
                std::vector<Tensor<2, dim>> empty;
                interface_points->swap(empty);
              }

            // Processor 0 receives data from the other processors
            if (Utilities::MPI::this_mpi_process(mpi_communicator) == 0)
              {
                std::vector<double> receive_data(2 * dim * receive_count[i]);
                int                 ierr = MPI_Recv(&receive_data[0],
                                    receive_data.size(),
                                    MPI_DOUBLE,
                                    i,
                                    i,
                                    mpi_communicator,
                                    MPI_STATUSES_IGNORE);
                (void)ierr;
                Assert(ierr == MPI_SUCCESS, ExcInternalError());
                for (unsigned int j = 0; j < receive_count[i]; ++j)
                  {
                    Tensor<2, dim> point;
                    for (unsigned int d = 0; d < 2; ++d)
                      for (unsigned int e = 0; e < dim; ++e)
                        point[d][e] = receive_data[j * 2 * dim + d * dim + e];
                    interface_points->push_back(point);
                  }
              }
          }
      }

    return data;
  }


private:
  void
  move_surface_mesh()
  {
    Assert(use_auxiliary_surface_mesh, ExcNotImplemented());

   /*
   VectorTools::update_position_vector(navier_stokes_solver.time_stepping.step_size(),
                                        navier_stokes_solver.get_dof_handler_u(),
                                        navier_stokes_solver.mapping,
                                        navier_stokes_solver.solution.block(0),
                                        euler_dofhandler,
                                        *euler_mapping,
                                        euler_vector);
                                        */
    //levelset_at_surface_vector.reinit(euler_dofhandler.n_dofs());
    VectorTools::update_position_vector_level_set(level_set_solver.get_dof_handler(),
                                        navier_stokes_solver.mapping,
                                        level_set_solver.get_level_set_vector(),
                                        level_set_solver.get_normal_vector(),
                                        level_set_solver.get_eps_used(),
                                        euler_dofhandler,
                                        *euler_mapping,
                                        euler_vector,
                                        levelset_at_surface_vector,
                                        level_set_at_surface,
                                        level_set_at_surface_max, 
                                        level_set_at_surface_norm);   
                                                                   
  }

  void
  update_phases()
  {
    const auto density        = navier_stokes_solver.get_parameters().density;
    const auto density_diff   = navier_stokes_solver.get_parameters().density_diff;
    const auto viscosity      = navier_stokes_solver.get_parameters().viscosity;
    const auto viscosity_diff = navier_stokes_solver.get_parameters().viscosity_diff;

    if (density_diff == 0.0 && viscosity_diff == 0.0)
      return; // nothing to do

    double dummy;

    // TODO: select proper MatrixFree object and set right dof/quad index
    level_set_solver.get_matrix_free().template cell_loop<double, VectorType>(
      [&](const auto &matrix_free, auto &, const auto &src, auto macro_cells) {
        FEEvaluation<dim, -1, 0, 1, double> phi(matrix_free,
                                                LevelSetSolver<dim>::dof_index_ls,
                                                LevelSetSolver<dim>::quad_index_vel);

        for (unsigned int cell = macro_cells.first; cell < macro_cells.second; ++cell)
          {
            phi.reinit(cell);
            phi.gather_evaluate(src, EvaluationFlags::values);

            for (unsigned int q = 0; q < phi.n_q_points; ++q)
              {
                const auto indicator =
                  (phi.get_value(q) + 1.0) / 2.0; // TODO: fix indicator -> Heaviside

                navier_stokes_solver.get_matrix().begin_densities(cell)[q] =
                  density + density_diff * indicator;
                navier_stokes_solver.get_matrix().begin_viscosities(cell)[q] =
                  viscosity + viscosity_diff * indicator;
              }
          }
      },
      dummy,
      level_set_solver.get_level_set_vector());
  }

  void
  update_surface_tension()
  {
    // mixed level set
    if (use_auxiliary_surface_mesh && use_sharp_interface)
      compute_force_vector_sharp_interface(
        euler_dofhandler.get_triangulation(),
        *euler_mapping,
        QGauss<dim - 1>(euler_dofhandler.get_fe().degree + 1),
        navier_stokes_solver.mapping,
        level_set_solver.get_dof_handler(),
        navier_stokes_solver.get_dof_handler_u(),
        navier_stokes_solver.get_parameters().surface_tension,
        level_set_solver.get_normal_vector(),
        level_set_solver.get_curvature_vector(),
        navier_stokes_solver.user_rhs.block(0),
        level_set_solver.get_level_set_vector(),
        interface_points_mc,
        contour_tria);
    else if (!use_auxiliary_surface_mesh && use_sharp_interface)
      compute_force_vector_sharp_interface(
        QGauss<dim - 1>(2 /*TODO*/),
        navier_stokes_solver.mapping,
        level_set_solver.get_dof_handler(),
        navier_stokes_solver.get_dof_handler_u(),
        navier_stokes_solver.get_parameters().surface_tension,
        level_set_solver.get_normal_vector(),
        level_set_solver.get_curvature_vector(),
        level_set_solver.get_level_set_vector(),
        navier_stokes_solver.user_rhs.block(0));
    else if (!use_auxiliary_surface_mesh && !use_sharp_interface)
      compute_force_vector_regularized(
        level_set_solver.get_matrix_free(),
        LevelSetSolver<dim>::dof_index_ls,
        LevelSetSolver<dim>::dof_index_curvature,
        LevelSetSolver<dim>::dof_index_velocity,
        LevelSetSolver<dim>::quad_index_vel,
        navier_stokes_solver.get_parameters().surface_tension,
        level_set_solver.get_level_set_vector(),
        level_set_solver.get_curvature_vector(),
        navier_stokes_solver.user_rhs.block(0));
    else
      AssertThrow(false, ExcNotImplemented());
  }

  void
  update_gravity_force()
  {
    const auto gravity = navier_stokes_solver.get_parameters().gravity;

    const auto density      = navier_stokes_solver.get_parameters().density;
    const auto density_diff = navier_stokes_solver.get_parameters().density_diff;

    const bool zero_out = true;

    navier_stokes_solver.matrix_free->template cell_loop<VectorType, std::nullptr_t>(
      [&](const auto &matrix_free, auto &vec, const auto &, auto macro_cells) {
        FEEvaluation<dim, -1, 0, dim, double> phi(matrix_free,
                                                  LevelSetSolver<dim>::dof_index_velocity,
                                                  LevelSetSolver<dim>::quad_index_vel);

        for (unsigned int cell = macro_cells.first; cell < macro_cells.second; ++cell)
          {
            phi.reinit(cell);

            for (unsigned int q = 0; q < phi.n_q_points; ++q)
              {
                Tensor<1, dim, VectorizedArray<double>> force;

                force[dim - 1] -=
                  gravity *
                  (density_diff == 0.0 ?
                     VectorizedArray<double>(density) :
                     navier_stokes_solver.get_matrix().begin_densities(cell)[q]);
                phi.submit_value(force, q);
              }
            phi.integrate_scatter(true, false, vec);
          }
      },
      navier_stokes_solver.user_rhs.block(0),
      nullptr,
      zero_out);
  }

  void
  set_adaptive_time_step(const double norm_velocity) const
  {
    // Evaluate the time step according to the stability condition.

    const double cfl       = navier_stokes_solver.get_parameters().time_stepping_cfl;
    const double rho_2     = navier_stokes_solver.get_parameters().viscosity_diff + navier_stokes_solver.get_parameters().viscosity;
    const double coef_2    = navier_stokes_solver.get_parameters().time_stepping_coef2;
    const double sigma_val = navier_stokes_solver.get_parameters().surface_tension;

    double new_time_step =
      1 /
      (1 / (cfl * level_set_solver.minimal_edge_length / norm_velocity) +
      1 / (coef_2 * std::sqrt(rho_2 / sigma_val) * std::pow(level_set_solver.minimal_edge_length, 1.5)));

    // hand this step to the timer stepper. The time stepper will make sure that
    // the time step does not change too rapidly from one iteration to the next
    // and also be within the bounds set in the parameter file.
    navier_stokes_solver.time_stepping.set_time_step(new_time_step);
  }

  //TODO yesterday
  std::vector<Point<dim>>
  get_level_set_interface() 
  {
    return interface_points_mc;
  }

  const bool use_auxiliary_surface_mesh;
  const bool use_sharp_interface;

  // compute bubble statistics
  std::pair<double, double>              concentration;
  mutable std::pair<double, double>      last_concentration_range;

  //for norm of level set
  VectorType                             levelset_at_surface_vector;
  std::vector<double>                    level_set_at_surface;
  std::vector<double>                    level_set_at_surface_max, level_set_at_surface_norm;
  
  // background mesh
  NavierStokes<dim> & navier_stokes_solver;
  LevelSetSolver<dim> level_set_solver;
  //TwoPhaseBaseAlgorithm<dim> & two_phase_solver;
  std::vector<Point<dim>>     interface_points_mc;
  Triangulation<dim - 1, dim> contour_tria;

  // surface mesh
  DoFHandler<dim - 1, dim>               euler_dofhandler;
  VectorType                             euler_vector;
  std::shared_ptr<Mapping<dim - 1, dim>> euler_mapping;
  
};

#endif
