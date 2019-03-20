//
//  CubismUP_2D
//  Copyright (c) 2018 CSE-Lab, ETH Zurich, Switzerland.
//  Distributed under the terms of the MIT license.
//
//  Created by Guido Novati (novatig@ethz.ch).
//


#include "PressureVarRho.h"
#include "../Poisson/FFTW_freespace.h"
#ifdef HYPREFFT
#include "../Poisson/HYPREdirichlet.h"
#endif
#include "../Poisson/FFTW_dirichlet.h"
#include "../Poisson/FFTW_periodic.h"
#ifdef CUDAFFT
#include "../Poisson/CUDA_all.h"
#endif

template<typename T>
static inline T mean(const T A, const T B)
{
  return 0.5*(A+B);
}

static inline PoissonSolver * makeSolver(SimulationData& sim)
{
  #ifdef HYPREFFT
  if (sim.poissonType == "hypre")
    return static_cast<PoissonSolver*>(new HYPREdirichlet(sim));
  else
  #endif
  if (sim.poissonType == "periodic")
    return static_cast<PoissonSolver*>(new FFTW_periodic(sim));
  else
  if (sim.poissonType == "cosine")
    return static_cast<PoissonSolver*>(new FFTW_dirichlet(sim));
  #ifdef CUDAFFT
  else
  if (sim.poissonType == "cuda-periodic")
    return static_cast<PoissonSolver*>(new CUDA_periodic(sim));
  else
  if (sim.poissonType == "cuda-freespace")
    return static_cast<PoissonSolver*>(new CUDA_freespace(sim));
  #endif
  else
    return static_cast<PoissonSolver*>(new FFTW_freespace(sim));
}

void PressureVarRho::fadeoutBorder(const double dt) const
{
  static constexpr int Z = 8, buffer = 8;
  const Real h = sim.getH(), iWidth = 1/(buffer*h);
  const Real extent[2] = {sim.bpdx/ (Real) std::max(sim.bpdx, sim.bpdy),
                          sim.bpdy/ (Real) std::max(sim.bpdx, sim.bpdy)};
  const auto _is_touching = [&] (const BlockInfo& i) {
    Real min_pos[2], max_pos[2];
    i.pos(max_pos, VectorBlock::sizeX-1, VectorBlock::sizeY-1);
    i.pos(min_pos, 0, 0);
    const bool touchN = (Z+buffer)*h >= extent[1] - max_pos[1];
    const bool touchE = (Z+buffer)*h >= extent[0] - max_pos[0];
    const bool touchS = (Z+buffer)*h >= min_pos[1];
    const bool touchW = (Z+buffer)*h >= min_pos[0];
    return touchN || touchE || touchS || touchW;
  };

  #pragma omp parallel for schedule(dynamic)
  for (size_t i=0; i < Nblocks; i++)
  {
    if( not _is_touching(tmpInfo[i]) ) continue;
    ScalarBlock& __restrict__ RHS = *(ScalarBlock*)tmpInfo[i].ptrBlock;
    for(int iy=0; iy<VectorBlock::sizeY; ++iy)
    for(int ix=0; ix<VectorBlock::sizeX; ++ix)
    {
      Real p[2];
      tmpInfo[i].pos(p, ix, iy);
      const Real arg1= std::max((Real)0, (Z+buffer)*h -(extent[0]-p[0]) );
      const Real arg2= std::max((Real)0, (Z+buffer)*h -(extent[1]-p[1]) );
      const Real arg3= std::max((Real)0, (Z+buffer)*h -p[0] );
      const Real arg4= std::max((Real)0, (Z+buffer)*h -p[1] );
      const Real dist= std::min(std::max({arg1, arg2, arg3, arg4}), buffer*h);
      //RHS(ix, iy).s = std::max(1-factor, 1 - factor*std::pow(dist*iWidth, 2));
      RHS(ix, iy).s *= 1 - std::pow(dist*iWidth, 2);
    }
  }
};

void PressureVarRho::updatePressureRHS(const double dt) const
{
  const Real h = sim.getH(), facDiv = 0.5*rho0*h/dt;

  #pragma omp parallel
  {
    static constexpr int stenBeg[3] = {-1,-1, 0}, stenEnd[3] = { 2, 2, 1};
    VectorLab velLab;  velLab.prepare( *(sim.vel),    stenBeg, stenEnd, 0);
    VectorLab uDefLab; uDefLab.prepare(*(sim.uDef),   stenBeg, stenEnd, 0);
    ScalarLab presLab; presLab.prepare(*(sim.pres),   stenBeg, stenEnd, 0);
    ScalarLab pOldLab; pOldLab.prepare(*(sim.pOld),   stenBeg, stenEnd, 0);
    ScalarLab iRhoLab; iRhoLab.prepare(*(sim.invRho), stenBeg, stenEnd, 0);
    #pragma omp for schedule(static)
    for (size_t i=0; i < Nblocks; i++)
    {
       velLab.load( velInfo[i], 0); uDefLab.load(uDefInfo[i], 0);
      presLab.load(presInfo[i], 0); pOldLab.load(pOldInfo[i], 0);
      iRhoLab.load(iRhoInfo[i], 0);
      const VectorLab  & __restrict__ V   =  velLab;
      const VectorLab  & __restrict__ UDEF= uDefLab;
      const ScalarLab  & __restrict__ P   = presLab;
      const ScalarLab  & __restrict__ pOld= pOldLab;
      const ScalarLab  & __restrict__ IRHO= iRhoLab;
      const ScalarBlock& __restrict__ CHI = *(ScalarBlock*)chiInfo[i].ptrBlock;
            ScalarBlock& __restrict__ TMP = *(ScalarBlock*)tmpInfo[i].ptrBlock;
      for(int iy=0; iy<VectorBlock::sizeY; ++iy)
      for(int ix=0; ix<VectorBlock::sizeX; ++ix)
      {
        const Real pNextN = 2*P(ix+1, iy  ).s - pOld(ix+1, iy  ).s;
        const Real pNextS = 2*P(ix-1, iy  ).s - pOld(ix-1, iy  ).s;
        const Real pNextE = 2*P(ix  , iy+1).s - pOld(ix  , iy+1).s;
        const Real pNextW = 2*P(ix  , iy-1).s - pOld(ix  , iy-1).s;
        const Real pNextC = 2*P(ix  , iy  ).s - pOld(ix  , iy  ).s;
        const Real rN = (1 -rho0 * mean(IRHO(ix+1,iy).s, IRHO(ix,iy).s));
        const Real rS = (1 -rho0 * mean(IRHO(ix-1,iy).s, IRHO(ix,iy).s));
        const Real rE = (1 -rho0 * mean(IRHO(ix,iy+1).s, IRHO(ix,iy).s));
        const Real rW = (1 -rho0 * mean(IRHO(ix,iy-1).s, IRHO(ix,iy).s));
        // gradP at midpoints, skip factor 1/h, but skip multiply by h^2 later
        const Real dN = pNextN - pNextC, dS = pNextC - pNextS;
        const Real dE = pNextE - pNextC, dW = pNextC - pNextW;
        // hatPfac=div((1-1/rho^*) gradP), div gives missing 1/h to make h^2
        const Real hatPfac = rE*dE - rW*dW + rN*dN - rS*dS;
        const Real divVx  = V(ix+1,iy).u[0]    - V(ix-1,iy).u[0];
        const Real divVy  = V(ix,iy+1).u[1]    - V(ix,iy-1).u[1];
        const Real divUSx = UDEF(ix+1,iy).u[0] - UDEF(ix-1,iy).u[0];
        const Real divUSy = UDEF(ix,iy+1).u[1] - UDEF(ix,iy-1).u[1];
        const Real rhsDiv = divVx+divVy - CHI(ix,iy).s * (divUSx+divUSy);
        TMP(ix, iy).s = facDiv*rhsDiv + hatPfac;
        //TMP(ix, iy).s = - CHI(ix,iy).s *facDiv* (divUSx+divUSy);
      }
    }
  }
}

void PressureVarRho::pressureCorrection(const double dt) const
{
  const Real h = sim.getH(), pFac = -0.5*dt/h, invRho0 = 1 / rho0;

  #pragma omp parallel
  {
    static constexpr int stenBeg[3] = {-1,-1, 0}, stenEnd[3] = { 2, 2, 1};
    ScalarLab  tmpLab;  tmpLab.prepare(*(sim.tmp ), stenBeg, stenEnd, 0);
    ScalarLab presLab; presLab.prepare(*(sim.pres), stenBeg, stenEnd, 0);
    ScalarLab pOldLab; pOldLab.prepare(*(sim.pOld), stenBeg, stenEnd, 0);

    #pragma omp for schedule(static)
    for (size_t i=0; i < Nblocks; i++)
    {
       tmpLab.load( tmpInfo[i],0);
      presLab.load(presInfo[i],0);
      pOldLab.load(pOldInfo[i],0);
      const ScalarLab  &__restrict__ Pcur =  tmpLab;
      const ScalarLab  &__restrict__ Pold = presLab;
      const ScalarLab  &__restrict__ Podr = pOldLab;
            VectorBlock&__restrict__   V = *(VectorBlock*) velInfo[i].ptrBlock;
      const ScalarBlock&__restrict__ IRHO= *(ScalarBlock*)iRhoInfo[i].ptrBlock;

      for(int iy=0; iy<VectorBlock::sizeY; ++iy)
      for(int ix=0; ix<VectorBlock::sizeX; ++ix)
      {
        const Real pNextN = 2*Pold(ix+1,iy).s - Podr(ix+1,iy).s;
        const Real pNextS = 2*Pold(ix-1,iy).s - Podr(ix-1,iy).s;
        const Real pNextE = 2*Pold(ix,iy+1).s - Podr(ix,iy+1).s;
        const Real pNextW = 2*Pold(ix,iy-1).s - Podr(ix,iy-1).s;
        // update vel field after most recent force and pressure response:
        V(ix,iy).u[0] += pFac * (Pcur(ix+1,iy).s - Pcur(ix-1,iy).s) * invRho0;
        V(ix,iy).u[1] += pFac * (Pcur(ix,iy+1).s - Pcur(ix,iy-1).s) * invRho0;
        V(ix,iy).u[0] += pFac * (pNextE-pNextW) * (IRHO(ix,iy).s - invRho0);
        V(ix,iy).u[1] += pFac * (pNextN-pNextS) * (IRHO(ix,iy).s - invRho0);
      }
    }
  }
}

void PressureVarRho::operator()(const double dt)
{
  sim.startProfiler("Prhs");
  updatePressureRHS(dt);
  fadeoutBorder(dt);
  sim.stopProfiler();

  pressureSolver->solve(tmpInfo, tmpInfo);

  sim.startProfiler("PCorrect");
  pressureCorrection(dt);
  sim.stopProfiler();

  #pragma omp parallel for schedule(static)
  for (size_t i=0; i < Nblocks; i++)
  {
    const ScalarBlock & __restrict__ Pn = *(ScalarBlock*)  tmpInfo[i].ptrBlock;
          ScalarBlock & __restrict__ Pc = *(ScalarBlock*) presInfo[i].ptrBlock;
          ScalarBlock & __restrict__ Po = *(ScalarBlock*) pOldInfo[i].ptrBlock;
    Po.copy(Pc);
    Pc.copy(Pn);
  }
}

PressureVarRho::PressureVarRho(SimulationData& s) : Operator(s), pressureSolver( makeSolver(s) )  { }

PressureVarRho::~PressureVarRho() {
    delete pressureSolver;
}