/*
** dobjgc.cpp
** The garbage collector. Based largely on Lua's.
**
**---------------------------------------------------------------------------
** Copyright 2008-2022 Marisa Heit
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/
/******************************************************************************
* Copyright (C) 1994-2008 Lua.org, PUC-Rio.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
******************************************************************************/

// HEADER FILES ------------------------------------------------------------

#include <limits>

#include "dobject.h"
#include "templates.h"
#include "b_bot.h"
#include "p_local.h"
#include "g_game.h"
#include "a_sharedglobal.h"
#include "sbar.h"
#include "stats.h"
#include "c_dispatch.h"
#include "s_sndseq.h"
#include "r_data/r_interpolate.h"
#include "doomstat.h"
#include "m_argv.h"
#include "po_man.h"
#include "autosegs.h"
#include "v_video.h"
#include "textures/textures.h"
#include "r_utility.h"
#include "menu/menu.h"
#include "intermission/intermission.h"
#include "g_levellocals.h"
#include "events.h"

// MACROS ------------------------------------------------------------------

/*
@@ DEFAULT_GCPAUSE defines the default pause between garbage-collector cycles
@* as a percentage.
** CHANGE it if you want the GC to run faster or slower (higher values
** mean larger pauses which mean slower collection.) You can also change
** this value dynamically.
*/
#define DEFAULT_GCPAUSE		150	// 150% (wait for memory to increase by half before next GC)

/*
@@ DEFAULT_GCMUL defines the default speed of garbage collection relative to
@* memory allocation as a percentage.
** CHANGE it if you want to change the granularity of the garbage
** collection. (Higher values mean coarser collections. 0 represents
** infinity, where each step performs a full collection.) You can also
** change this value dynamically.
*/
#define DEFAULT_GCMUL		200 // GC runs 'double the speed' of memory allocation

// Minimum step size
#define GCSTEPSIZE		(sizeof(DObject) * 16)
#define SECTORSTEPSIZE	32
#define POLYSTEPSIZE 120
#define SIDEDEFSTEPSIZE 240

// Maximum number of elements to sweep in a single step
#define GCSWEEPMAX		40

// Cost of sweeping one element (the size of a small object divided by
// some adjust for the sweep speed)
#define GCSWEEPCOST		(sizeof(DObject) / 4)

// Cost of calling of one destructor
#define GCFINALIZECOST	100

// TYPES -------------------------------------------------------------------

// This object is responsible for marking sectors during the propagate
// stage. In case there are many, many sectors, it lets us break them
// up instead of marking them all at once.
class DSectorMarker : public DObject
{
	DECLARE_CLASS(DSectorMarker, DObject)
public:
	DSectorMarker() : SecNum(0),PolyNum(0),SideNum(0) {}
	size_t PropagateMark();
	int SecNum;
	int PolyNum;
	int SideNum;
};

IMPLEMENT_CLASS(DSectorMarker, false, false)

// EXTERNAL FUNCTION PROTOTYPES --------------------------------------------

// PUBLIC FUNCTION PROTOTYPES ----------------------------------------------

// PRIVATE FUNCTION PROTOTYPES ---------------------------------------------

static size_t CalcStepSize();

// EXTERNAL DATA DECLARATIONS ----------------------------------------------

extern DThinker *NextToThink;

// PUBLIC DATA DEFINITIONS -------------------------------------------------

namespace GC
{
size_t AllocBytes;
size_t Threshold;
size_t Estimate;
DObject *Gray;
DObject *Root;
DObject *SoftRoots;
DObject **SweepPos;
uint32_t CurrentWhite = OF_White0 | OF_Fixed;
EGCState State = GCS_Pause;
int Pause = DEFAULT_GCPAUSE;
int StepMul = DEFAULT_GCMUL;
int StepCount;
uint64_t CheckTime;
bool FinalGC;

// PRIVATE DATA DEFINITIONS ------------------------------------------------

static DSectorMarker *SectorMarker;
static int LastCollectTime;		// Time last time collector finished
static size_t LastCollectAlloc;	// Memory allocation when collector finished
static size_t MinStepSize;		// Cover at least this much memory per step

// CODE --------------------------------------------------------------------

//==========================================================================
//
// SetThreshold
//
// Sets the new threshold after a collection is finished.
//
//==========================================================================

void SetThreshold()
{
	Threshold = (Estimate / 100) * Pause;
}

//==========================================================================
//
// PropagateMark
//
// Marks the top-most gray object black and marks all objects it points to
// gray.
//
//==========================================================================

size_t PropagateMark()
{
	DObject *obj = Gray;
	assert(obj->IsGray());
	obj->Gray2Black();
	Gray = obj->GCNext;
	return !(obj->ObjectFlags & OF_EuthanizeMe) ? obj->PropagateMark() :
		obj->GetClass()->Size;
}

//==========================================================================
//
// PropagateAll
//
// Empties the gray list by propagating every single object in it.
//
//==========================================================================

static size_t PropagateAll()
{
	size_t m = 0;
	while (Gray != NULL)
	{
		m += PropagateMark();
	}
	return m;
}

//==========================================================================
//
// SweepList
//
// Runs a limited sweep on a list, returning the position in the list just
// after the last object swept.
//
//==========================================================================

static DObject **SweepList(DObject **p, size_t count, size_t *finalize_count)
{
	DObject *curr;
	int deadmask = OtherWhite();
	size_t finalized = 0;

	while ((curr = *p) != NULL && count-- > 0)
	{
		if ((curr->ObjectFlags ^ OF_WhiteBits) & deadmask)	// not dead?
		{
			assert(!curr->IsDead() || (curr->ObjectFlags & OF_Fixed));
			curr->MakeWhite();	// make it white (for next cycle)
			p = &curr->ObjNext;
		}
		else	// must erase 'curr'
		{
			assert(curr->IsDead());
			*p = curr->ObjNext;
			if (!(curr->ObjectFlags & OF_EuthanizeMe))
			{	// The object must be destroyed before it can be finalized.
				// Note that thinkers must already have been destroyed. If they get here without
				// having been destroyed first, it means they somehow became unattached from the
				// thinker lists. If I don't maintain the invariant that all live thinkers must
				// be in a thinker list, then I need to add write barriers for every time a
				// thinker pointer is changed. This seems easier and perfectly reasonable, since
				// a live thinker that isn't on a thinker list isn't much of a thinker.

				// However, this can happen during deletion of the thinker list while cleaning up
				// from a savegame error so we can't assume that any thinker that gets here is an error.

				curr->Destroy();
			}
			curr->ObjectFlags |= OF_Cleanup;
			delete curr;
			finalized++;
		}
	}
	if (finalize_count != NULL)
	{
		*finalize_count = finalized;
	}
	return p;
}

//==========================================================================
//
// Mark
//
// Mark a single object gray.
//
//==========================================================================

void Mark(DObject **obj)
{
	DObject *lobj = *obj;

	//assert(lobj == nullptr || !(lobj->ObjectFlags & OF_Released));
	if (lobj != nullptr && !(lobj->ObjectFlags & OF_Released))
	{
		if (lobj->ObjectFlags & OF_EuthanizeMe)
		{
			*obj = (DObject *)NULL;
		}
		else if (lobj->IsWhite())
		{
			lobj->White2Gray();
			lobj->GCNext = Gray;
			Gray = lobj;
		}
	}
}

//==========================================================================
//
// MarkArray
//
// Mark an array of objects gray.
//
//==========================================================================

void MarkArray(DObject **obj, size_t count)
{
	for (size_t i = 0; i < count; ++i)
	{
		Mark(obj[i]);
	}
}

//==========================================================================
//
// CalcStepSize
//
// Decide how big a step should be based, depending on how long it took to
// allocate up to the threshold from the amount left after the previous
// collection.
//
//==========================================================================

static size_t CalcStepSize()
{
	int time_passed = CheckTime - LastCollectTime;
	auto alloc = MIN(LastCollectAlloc, Estimate);
	size_t bytes_gained = AllocBytes > alloc ? AllocBytes - alloc : 0;
	return (StepMul > 0 && time_passed > 0)
		? std::max<size_t>(GCSTEPSIZE, bytes_gained / time_passed * StepMul / 100)
		: std::numeric_limits<size_t>::max() / 2;		// no limit
}

//==========================================================================
//
// MarkRoot
//
// Mark the root set of objects.
//
//==========================================================================

static void MarkRoot()
{
	int i;

	Gray = NULL;
	Mark(StatusBar);
	M_MarkMenus();
	Mark(DIntermissionController::CurrentIntermission);
	DThinker::MarkRoots();
	FCanvasTextureInfo::Mark();
	Mark(E_FirstEventHandler);
	Mark(E_LastEventHandler);
	level.Mark();

	// Mark players.
	for (i = 0; i < MAXPLAYERS; i++)
	{
		if (playeringame[i])
			players[i].PropagateMark();
	}
	// Mark sound sequences.
	DSeqNode::StaticMarkHead();
	// Mark sectors.
	if (SectorMarker == nullptr && level.sectors.Size() > 0)
	{
		SectorMarker = Create<DSectorMarker>();
	}
	else if (level.sectors.Size() == 0)
	{
		SectorMarker = nullptr;
	}
	else
	{
		SectorMarker->SecNum = 0;
	}
	Mark(SectorMarker);
	Mark(interpolator.Head);
	// Mark bot stuff.
	Mark(bglobal.firstthing);
	Mark(bglobal.body1);
	Mark(bglobal.body2);
	// NextToThink must not be freed while thinkers are ticking.
	Mark(NextToThink);
	// Mark soft roots.
	if (SoftRoots != NULL)
	{
		DObject **probe = &SoftRoots->ObjNext;
		while (*probe != NULL)
		{
			DObject *soft = *probe;
			probe = &soft->ObjNext;
			if ((soft->ObjectFlags & (OF_Rooted | OF_EuthanizeMe)) == OF_Rooted)
			{
				Mark(soft);
			}
		}
	}
	// Time to propagate the marks.
	State = GCS_Propagate;
	StepCount = 0;
}

//==========================================================================
//
// Atomic
//
// If there were any propagations that needed to be done atomicly, they
// would go here. It also sets things up for the sweep state.
//
//==========================================================================

static void Atomic()
{
	// Flip current white
	CurrentWhite = OtherWhite();
	SweepPos = &Root;
	State = GCS_Sweep;
	Estimate = AllocBytes;

	// Now that we are about to start a sweep, establish a baseline minimum
	// step size for how much memory we want to sweep each CheckGC().
	MinStepSize = CalcStepSize();
}

//==========================================================================
//
// SingleStep
//
// Performs one step of the collector.
//
//==========================================================================

static size_t SingleStep()
{
	switch (State)
	{
	case GCS_Pause:
		MarkRoot();		// Start a new collection
		return 0;

	case GCS_Propagate:
		if (Gray != NULL)
		{
			return PropagateMark();
		}
		else
		{ // no more gray objects
			Atomic();	// finish mark phase
			return 0;
		}

	case GCS_Sweep: {
		size_t old = AllocBytes;
		size_t finalize_count;
		SweepPos = SweepList(SweepPos, GCSWEEPMAX, &finalize_count);
		if (*SweepPos == NULL)
		{ // Nothing more to sweep?
			State = GCS_Finalize;
		}
		//assert(old >= AllocBytes);
		Estimate -= MAX<size_t>(0, old - AllocBytes);
		return (GCSWEEPMAX - finalize_count) * GCSWEEPCOST + finalize_count * GCFINALIZECOST;
	  }

	case GCS_Finalize:
		State = GCS_Pause;		// end collection
		LastCollectAlloc = AllocBytes;
		LastCollectTime = CheckTime;
		return 0;

	default:
		assert(0);
		return 0;
	}
}

//==========================================================================
//
// Step
//
// Performs enough single steps to cover GCSTEPSIZE * StepMul% bytes of
// memory.
//
//==========================================================================

void Step()
{
	// We recalculate a step size in case the rate of allocation went up
	// since we started sweeping because we don't want to fall behind.
	// However, we also don't want to go slower than what was decided upon
	// when the sweep began if the rate of allocation has slowed.
	size_t lim = MAX(CalcStepSize(), MinStepSize);
	do
	{
		size_t done = SingleStep();
		if (done < lim)
		{
			lim -= done;
		}
		else
		{
			lim = 0;
		}
	} while (lim && State != GCS_Pause);
	if (State != GCS_Pause)
	{
		Threshold = AllocBytes;
	}
	else
	{
		assert(AllocBytes >= Estimate);
		SetThreshold();
	}
	StepCount++;
}

//==========================================================================
//
// FullGC
//
// Collects everything in one fell swoop.
//
//==========================================================================

void FullGC()
{
	if (State <= GCS_Propagate)
	{
		// Reset sweep mark to sweep all elements (returning them to white)
		SweepPos = &Root;
		// Reset other collector lists
		Gray = NULL;
		State = GCS_Sweep;
	}
	// Finish any pending sweep phase
	while (State != GCS_Finalize)
	{
		SingleStep();
	}
	MarkRoot();
	while (State != GCS_Pause)
	{
		SingleStep();
	}
	SetThreshold();
}

//==========================================================================
//
// Barrier
//
// Implements a write barrier to maintain the invariant that a black node
// never points to a white node by making the node pointed at gray.
//
//==========================================================================

void Barrier(DObject *pointing, DObject *pointed)
{
	assert(pointing == NULL || (pointing->IsBlack() && !pointing->IsDead()));
	assert(pointed->IsWhite() && !pointed->IsDead());
	assert(State != GCS_Finalize && State != GCS_Pause);
	assert(!(pointed->ObjectFlags & OF_Released));	// if a released object gets here, something must be wrong.
	if (pointed->ObjectFlags & OF_Released) return;	// don't do anything with non-GC'd objects.
	// The invariant only needs to be maintained in the propagate state.
	if (State == GCS_Propagate)
	{
		pointed->White2Gray();
		pointed->GCNext = Gray;
		Gray = pointed;
	}
	// In other states, we can mark the pointing object white so this
	// barrier won't be triggered again, saving a few cycles in the future.
	else if (pointing != NULL)
	{
		pointing->MakeWhite();
	}
}

void DelSoftRootHead()
{
	if (SoftRoots != NULL)
	{
		// Don't let the destructor print a warning message
		SoftRoots->ObjectFlags |= OF_YesReallyDelete;
		delete SoftRoots;
	}
	SoftRoots = NULL;
}

//==========================================================================
//
// AddSoftRoot
//
// Marks an object as a soft root. A soft root behaves exactly like a root
// in MarkRoot, except it can be added at run-time.
//
//==========================================================================

void AddSoftRoot(DObject *obj)
{
	DObject **probe;

	// Are there any soft roots yet?
	if (SoftRoots == NULL)
	{
		// Create a new object to root the soft roots off of, and stick
		// it at the end of the object list, so we know that anything
		// before it is not a soft root.
		SoftRoots = Create<DObject>();
		SoftRoots->ObjectFlags |= OF_Fixed;
		probe = &Root;
		while (*probe != NULL)
		{
			probe = &(*probe)->ObjNext;
		}
		Root = SoftRoots->ObjNext;
		SoftRoots->ObjNext = NULL;
		*probe = SoftRoots;
	}
	// Mark this object as rooted and move it after the SoftRoots marker.
	probe = &Root;
	while (*probe != NULL && *probe != obj)
	{
		probe = &(*probe)->ObjNext;
	}
	*probe = (*probe)->ObjNext;
	obj->ObjNext = SoftRoots->ObjNext;
	SoftRoots->ObjNext = obj;
	obj->ObjectFlags |= OF_Rooted;
	WriteBarrier(obj);
}

//==========================================================================
//
// DelSoftRoot
//
// Unroots an object so that it must be reachable or it will get collected.
//
//==========================================================================

void DelSoftRoot(DObject *obj)
{
	DObject **probe;

	if (!(obj->ObjectFlags & OF_Rooted))
	{ // Not rooted, so nothing to do.
		return;
	}
	obj->ObjectFlags &= ~OF_Rooted;
	// Move object out of the soft roots part of the list.
	probe = &SoftRoots;
	while (*probe != NULL && *probe != obj)
	{
		probe = &(*probe)->ObjNext;
	}
	if (*probe == obj)
	{
		*probe = obj->ObjNext;
		obj->ObjNext = Root;
		Root = obj;
	}
}

}

//==========================================================================
//
// DSectorMarker :: PropagateMark
//
// Propagates marks across a few sectors and reinserts itself into the
// gray list if it didn't do them all.
//
//==========================================================================

size_t DSectorMarker::PropagateMark()
{
	int i;
	int marked = 0;
	bool moretodo = false;
	int numsectors = level.sectors.Size();

	for (i = 0; i < SECTORSTEPSIZE && SecNum + i < numsectors; ++i)
	{
		sector_t *sec = &level.sectors[SecNum + i];
		GC::Mark(sec->SoundTarget);
		GC::Mark(sec->SecActTarget);
		GC::Mark(sec->floordata);
		GC::Mark(sec->ceilingdata);
		GC::Mark(sec->lightingdata);
		for(int j=0;j<4;j++) GC::Mark(sec->interpolations[j]);
	}
	marked += i * sizeof(sector_t);
	if (SecNum + i < numsectors)
	{
		SecNum += i;
		moretodo = true;
	}

	if (!moretodo && polyobjs != NULL)
	{
		for (i = 0; i < POLYSTEPSIZE && PolyNum + i < po_NumPolyobjs; ++i)
		{
			GC::Mark(polyobjs[PolyNum + i].interpolation);
		}
		marked += i * sizeof(FPolyObj);
		if (PolyNum + i < po_NumPolyobjs)
		{
			PolyNum += i;
			moretodo = true;
		}
	}
	if (!moretodo && level.sides.Size() > 0)
	{
		for (i = 0; i < SIDEDEFSTEPSIZE && SideNum + i < (int)level.sides.Size(); ++i)
		{
			side_t *side = &level.sides[SideNum + i];
			for(int j=0;j<3;j++) GC::Mark(side->textures[j].interpolation);
		}
		marked += i * sizeof(side_t);
		if (SideNum + i < (int)level.sides.Size())
		{
			SideNum += i;
			moretodo = true;
		}
	}
	// If there are more sectors to mark, put ourself back into the gray
	// list.
	if (moretodo)
	{
		Black2Gray();
		GCNext = GC::Gray;
		GC::Gray = this;
	}
	return marked;
}

//==========================================================================
//
// STAT gc
//
// Provides information about the current garbage collector state.
//
//==========================================================================

ADD_STAT(gc)
{
	static const char *StateStrings[] = {
		"  Pause  ",
		"Propagate",
		"  Sweep  ",
		"Finalize " };
	FString out;
	out.Format("[%s] Alloc:%6zuK  Thresh:%6zuK  Est:%6zuK  Steps: %d  %zuK",
		StateStrings[GC::State],
		(GC::AllocBytes + 1023) >> 10,
		(GC::Threshold + 1023) >> 10,
		(GC::Estimate + 1023) >> 10,
		GC::StepCount,
		(GC::MinStepSize + 1023) >> 10);
	return out;
}

//==========================================================================
//
// CCMD gc
//
// Controls various aspects of the collector.
//
//==========================================================================

CCMD(gc)
{
	if (argv.argc() == 1)
	{
		Printf ("Usage: gc stop|now|full|count|pause [size]|stepmul [size]\n");
		return;
	}
	if (stricmp(argv[1], "stop") == 0)
	{
		GC::Threshold = ~(size_t)0 - 2;
	}
	else if (stricmp(argv[1], "now") == 0)
	{
		GC::Threshold = GC::AllocBytes;
	}
	else if (stricmp(argv[1], "full") == 0)
	{
		GC::FullGC();
	}
	else if (stricmp(argv[1], "count") == 0)
	{
		int cnt = 0;
		for (DObject *obj = GC::Root; obj; obj = obj->ObjNext, cnt++);
		Printf("%d active objects counted\n", cnt);
	}
	else if (stricmp(argv[1], "pause") == 0)
	{
		if (argv.argc() == 2)
		{
			Printf ("Current GC pause is %d\n", GC::Pause);
		}
		else
		{
			GC::Pause = MAX(1,atoi(argv[2]));
		}
	}
	else if (stricmp(argv[1], "stepmul") == 0)
	{
		if (argv.argc() == 2)
		{
			Printf ("Current GC stepmul is %d\n", GC::StepMul);
		}
		else
		{
			GC::StepMul = MAX(100, atoi(argv[2]));
		}
	}
}

