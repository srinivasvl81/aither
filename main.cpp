/*  An open source Navier-Stokes CFD solver.
    Copyright (C) 2015  Michael Nucci (michael.nucci@gmail.com)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>. */

#include <iostream>      // cout, cerr, endl
#include <vector>        // stl vector
#include <algorithm>     // max_element
#include <numeric>       // accumulate
#include <cfenv>         // exceptions
#include <ctime>         // clock
#include <string>        // stl string
#include "plot3d.hpp"
#include "vector3d.hpp"
#include "tensor.hpp"
#include "input.hpp"
#include "procBlock.hpp"
#include "inviscidFlux.hpp"
#include "viscousFlux.hpp"
#include "primVars.hpp"
#include "eos.hpp"
#include "boundaryConditions.hpp"
#include "output.hpp"
#include "matrix.hpp"
#include "parallel.hpp"
#include "turbulence.hpp"
#include "gradients.hpp"

using std::cout;
using std::cerr;
using std::endl;
using std::vector;
using std::clock_t;
using std::clock;

int main(int argc, char *argv[]) {
  // Initialize MPI and make calls to get number
  // of processors and rank of each processor
  int numProcs, rank;
  MPI_Init(&argc, &argv);
  MPI_Comm_size(MPI_COMM_WORLD, &numProcs);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  // Get MPI version
  int version, subversion;
  MPI_Get_version(&version, &subversion);
  if ( rank == ROOT ) {
    cout << "Using MPI Version " << version << "." << subversion << endl;
    cout << "Using " << numProcs << " processors" << endl;
  }
  MPI_Barrier(MPI_COMM_WORLD);

  cout << "Hello from processor " << rank << " of " << numProcs << "!" << endl;

  // Start clock to time simulation
  clock_t start;
  double duration;
  start = clock();

  // Enable exceptions so code won't run with NANs
  feenableexcept(FE_DIVBYZERO | FE_INVALID | FE_OVERFLOW);

  double totalCells = 0.0;
  input inputVars;
  decomposition decomp;
  int numProcBlock = 0;

  // Name of input file is the second argument (the executable being the first)
  string inputFile = argv[1];

  // Broadcast input file name to all names for portability
  BroadcastString(inputFile);

  // Parse input file
  inputVars.ReadInput(inputFile, rank);

  // Determine number of ghost cells
  int numGhost = 2;

  // Get equation of state
  idealGas eos(inputVars.Gamma(), inputVars.R());

  // Initialize sutherland's law for viscosity
  sutherland suth(inputVars.TRef(), inputVars.RRef(), inputVars.LRef(),
                  inputVars.PRef(), inputVars.VelRef(), eos);

  // Initialize state vector with nondimensional variables
  // Get reference speed of sound
  double aRef = eos.SoS(inputVars.PRef(), inputVars.RRef());
  primVars state;
  state.NondimensionalInitialize(eos, aRef, inputVars, suth.MuRef());

  vector<plot3dBlock> mesh;
  vector<interblock> connections;
  vector<procBlock> stateBlocks;

  if (rank == ROOT) {
    cout << "Number of equations: " << inputVars.NumEquations() << endl << endl;

    // Read grid
    mesh = ReadP3dGrid(inputVars.GridName(), inputVars.LRef(), totalCells);

    // Get BCs for blocks
    vector<boundaryConditions> bcs = inputVars.AllBC();

    // Decompose grid
    if (inputVars.DecompMethod() == "manual") {
      decomp = ManualDecomposition(mesh, bcs, numProcs);
    } else if (inputVars.DecompMethod() == "cubic") {
      decomp = CubicDecomposition(mesh, bcs, numProcs);
    } else {
      cerr << "ERROR: Domain decomposition method " << inputVars.DecompMethod()
           << " is not recognized!" << endl;
      exit(0);
    }

    // Get interblock BCs
    connections = GetInterblockBCs(bcs, mesh, decomp);

    // Could send proc3dblocks to processors here,
    // or initialize all on ROOT processor

    // Initialize the whole mesh with one state and assign ghost cells geometry
    stateBlocks.resize(mesh.size());
    for ( int ll = 0; ll < static_cast<int> (mesh.size()); ll++ ) {
      stateBlocks[ll] = procBlock(state, mesh[ll], decomp.ParentBlock(ll),
                                  numGhost, bcs[ll], ll, decomp.Rank(ll),
                                  decomp.LocalPosition(ll) );
      stateBlocks[ll].AssignGhostCellsGeom();
    }

    // Swap geometry for interblock BCs
    for ( unsigned int ii = 0; ii < connections.size(); ii++ ) {
      SwapSlice(connections[ii], stateBlocks[connections[ii].BlockFirst()],
                stateBlocks[connections[ii].BlockSecond()], true);
    }
    // Get ghost cell edge data
    for ( int ll = 0; ll < static_cast<int> (mesh.size()); ll++ ) {
      stateBlocks[ll].AssignGhostCellsGeomEdge();
    }

    cout << endl << "Solution Initialized" << endl;
    //---------------------------------------------------------------------
  }

  // Set MPI datatypes
  MPI_Datatype MPI_vec3d, MPI_cellData, MPI_procBlockInts,
      MPI_interblock, MPI_DOUBLE_5INT;
  SetDataTypesMPI(MPI_vec3d, MPI_cellData, MPI_procBlockInts,
                  MPI_interblock, MPI_DOUBLE_5INT);

  // Send number of procBlocks to all processors
  SendNumProcBlocks(decomp.NumBlocksOnAllProc(), numProcBlock);

  // Send procBlocks to appropriate processor
  vector<procBlock> localStateBlocks =
      SendProcBlocks(stateBlocks, rank, numProcBlock, MPI_cellData, MPI_vec3d);

  // Send connections to all processors
  SendConnections(connections, MPI_interblock);

  // Create operation
  MPI_Op MPI_MAX_LINF;
  MPI_Op_create(reinterpret_cast<MPI_User_function *> (MaxLinf), true,
                &MPI_MAX_LINF);

  //-----------------------------------------------------------------------

  // Column matrix of zeros
  genArray initial(0.0);

  // Preallocate vectors for old solution
  // Outermost vector for blocks, inner vector for cell in blocks,
  // genArray for variables in cell
  vector<vector<genArray> > solTimeN(numProcBlock);
  vector<vector<genArray> > solDeltaNm1(numProcBlock);

  // Initialize residual variables
  genArray residL2(0.0);  // l2 norm residuals
  genArray residL2First(0.0);  // l2 norm residuals to normalize by
  resid residLinf;  // linf residuals
  // matrix inversion residual (only for implicit runs)
  double matrixResid = 0.0;

  if (rank == ROOT) {
    // Write out cell centers grid file
    WriteCellCenter(inputVars.GridName(), stateBlocks, decomp,
                    inputVars.LRef());

    // Write out initial results
    WriteFun(inputVars.GridName(), stateBlocks, eos, suth, 0, decomp,
             inputVars);
    WriteRes(inputVars.GridName(), 0, inputVars.OutputFrequency());
  }

  for ( int nn = 0; nn < inputVars.Iterations(); nn++ ) {   // loop over time
    // Calculate cfl number
    inputVars.CalcCFL(nn);

    // loop over nonlinear iterations
    for ( int mm = 0; mm < inputVars.NonlinearIterations(); mm++ ) {
      // Get boundary conditions for all blocks
      GetBoundaryConditions(localStateBlocks, inputVars, eos, suth,
                            connections, rank, MPI_cellData);

      // Loop over number of blocks
      for ( int bb = 0; bb < static_cast<int> (localStateBlocks.size());
            bb++ ) {
        // Calculate inviscid fluxes
        localStateBlocks[bb].CalcInvFluxI(eos, inputVars);
        localStateBlocks[bb].CalcInvFluxJ(eos, inputVars);
        localStateBlocks[bb].CalcInvFluxK(eos, inputVars);

        // If viscous change ghost cells and calculate viscous fluxes
        if (inputVars.IsViscous()) {
          // Determine ghost cell values for viscous fluxes
          localStateBlocks[bb].AssignViscousGhostCells(inputVars, eos, suth);

          // Calculate gradients
          gradients grads(inputVars.IsTurbulent(), localStateBlocks[bb], eos);

          // Calculate viscous fluxes
          localStateBlocks[bb].CalcViscFluxI(suth, eos, inputVars, grads);
          localStateBlocks[bb].CalcViscFluxJ(suth, eos, inputVars, grads);
          localStateBlocks[bb].CalcViscFluxK(suth, eos, inputVars, grads);

          // If turblent, calculate source terms
          if (inputVars.IsTurbulent()) {
            // calculate source terms
            // localStateBlocks[bb].CalcSource();
          }
        }

        // Calculate the time step to use in the simulation
        // (either user specified or derived from CFL)
        localStateBlocks[bb].CalcBlockTimeStep(inputVars, aRef);

        // If implicit get old solution, reorder block, and use linear solver
        if (inputVars.IsImplicit()) {
          // Store time-n solution
          if (mm == 0) {  // first nonlinear iteration, save solution
            solTimeN[bb] = localStateBlocks[bb].GetCopyConsVars(eos);

            // At first iteration, resize vector for old solution
            // and calculate solution at time n=-1
            if (nn == 0) {
              solDeltaNm1[bb].resize(solTimeN[bb].size());
              localStateBlocks[bb].DeltaNMinusOne(solDeltaNm1[bb], solTimeN[bb],
                                                  eos, inputVars.Theta(),
                                                  inputVars.Zeta());
            }
          }

          // Add volume divided by time step term to time m minus time n values
          vector<genArray> solTimeMmN =
              localStateBlocks[bb].AddVolTime(
                  localStateBlocks[bb].GetCopyConsVars(eos),
                  solTimeN[bb], inputVars.Theta(), inputVars.Zeta());

          // Reorder block (by hyperplanes) for lusgs
          vector<vector3d<int> > reorder =
              HyperplaneReorder(localStateBlocks[bb].NumI(),
                                localStateBlocks[bb].NumJ(),
                                localStateBlocks[bb].NumK());

          // Reserve space for correction du
          vector<genArray> du(localStateBlocks[bb].NumCells(), initial);

          // Calculate correction (du)
          matrixResid += localStateBlocks[bb].LUSGS(reorder, du, solTimeMmN,
                                                    solDeltaNm1[bb], eos,
                                                    inputVars, suth);

          // Update solution
          localStateBlocks[bb].UpdateBlock(inputVars, inputVars.IsImplicit(),
                                           eos, aRef, du, residL2, residLinf);

          // Assign time n to time n-1 at end of nonlinear iterations
          if (inputVars.TimeIntegration() == "bdf2" &&
              mm == inputVars.NonlinearIterations() - 1 ) {
            localStateBlocks[bb].DeltaNMinusOne(solDeltaNm1[bb], solTimeN[bb],
                                                eos, inputVars.Theta(),
                                                inputVars.Zeta());
          }
        } else {  // explicit
          // Update solution
          vector<genArray> dummyCorrection(1);  // not used in explicit update
          localStateBlocks[bb].UpdateBlock(inputVars, inputVars.IsImplicit(),
                                           eos, aRef, dummyCorrection, residL2,
                                           residLinf);
        }

        // Zero residuals and wave speed
        localStateBlocks[bb].ResetResidWS();
      }  // loop for blocks ---------------------------------------------------

      // Get residuals from all processors
      residL2.GlobalReduceMPI(rank, inputVars.NumEquations());
      residLinf.GlobalReduceMPI(rank, MPI_DOUBLE_5INT, MPI_MAX_LINF);

      // Get matrix residuals from all processors
      if (rank == ROOT) {
        MPI_Reduce(MPI_IN_PLACE, &matrixResid, 1, MPI_DOUBLE, MPI_SUM,
                   ROOT, MPI_COMM_WORLD);
      } else {
        MPI_Reduce(&matrixResid, &matrixResid, 1, MPI_DOUBLE, MPI_SUM,
                   ROOT, MPI_COMM_WORLD);
      }

      if (rank == ROOT) {
        // Finish calculation of L2 norm of residual
        for ( int cc = 0; cc < inputVars.NumEquations(); cc++ ) {
          residL2[cc] = sqrt(residL2[cc]);
        }
        // Finish calculation of matrix residual
        matrixResid = sqrt(matrixResid/(totalCells * inputVars.NumEquations()));

        // Print out run information
        WriteResiduals(inputVars, residL2First, residL2, residLinf, matrixResid,
                       nn, mm);
      }

      // Reset residuals
      residL2.Zero();
      residLinf.Zero();
      matrixResid = 0.0;
    }  // loop for nonlinear iterations ---------------------------------------


    if ((nn+1) % inputVars.OutputFrequency() == 0) {  // write out function file
      // Send/recv solutions
      GetProcBlocks(stateBlocks, localStateBlocks, rank, MPI_cellData);

      if (rank == ROOT) {
        cout << "writing out function file at iteration " << nn << endl;
        // Write out function file
        WriteFun(inputVars.GridName(), stateBlocks, eos, suth, (nn+1),
                 decomp, inputVars);
        WriteRes(inputVars.GridName(), (nn+1), inputVars.OutputFrequency());
      }
    }
  }  // loop for time ----------------------------------------------------------


  if (rank == ROOT) {
    cout << endl;
    cout << "Program Complete" << endl;
    PrintTime();

    duration = (clock() - start)/static_cast<double> (CLOCKS_PER_SEC);
    cout << "Total Time: " << duration << " seconds" << endl;
  }

  // Free datatypes previously created
  FreeDataTypesMPI(MPI_vec3d, MPI_cellData, MPI_procBlockInts,
                   MPI_interblock, MPI_DOUBLE_5INT);
  MPI_Finalize();

  return 0;
}
