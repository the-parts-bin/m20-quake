// m20: per-region CPU counters (make PERF=1, build-perf/): cycles,
// instructions and four events (VexiiRiscv's --performance-counters 4,
// ../m20-core/rtl/VexiiRiscv.options), summed over each region's calls between
// M20_PerfReset and M20_PerfReport (the timedemo's start and end, cl_demo.c).
// Regions may nest; each one counts everything inside it.
#ifndef M20_PERF_H
#define M20_PERF_H

enum {
    M20_PERF_VIEW,          // R_RenderView: the whole 3D view
    M20_PERF_WORLD,         // R_RenderWorld: BSP walk, faces to edges
    M20_PERF_BENTS,         // R_DrawBEntitiesOnList: brush models to edges
    M20_PERF_SCAN,          // R_ScanEdges: edge sort and spans, with D_DrawSurfaces
    M20_PERF_DSURF,         // D_DrawSurfaces: the surfaces' spans, with surface building
    M20_PERF_BUILD,         // R_DrawSurface: surface building (lightmap x texture)
    M20_PERF_ENTS,          // R_DrawEntitiesOnList: alias models and sprites
    M20_PERF_VIEWMODEL,     // R_DrawViewModel: the weapon
    M20_PERF_PARTICLES,     // R_DrawParticles
    M20_PERF_N
};

#ifdef M20_PERF
void M20_PerfBegin(int r);
void M20_PerfEnd(int r);
void M20_PerfReset(void);
void M20_PerfReport(void);
#define M20_PERF_BEGIN(r) M20_PerfBegin(r)
#define M20_PERF_END(r)   M20_PerfEnd(r)
#else
#define M20_PERF_BEGIN(r) ((void)0)
#define M20_PERF_END(r)   ((void)0)
#define M20_PerfReset()   ((void)0)
#ifdef M20_PGO_GEN
void M20_PgoDump(void);     // make PGO=gen: the branch counters (m20_pgo.c)
#define M20_PerfReport()  M20_PgoDump()
#else
#define M20_PerfReport()  ((void)0)
#endif
#endif

#endif
