#include "postgres.h"

#include <math.h>

#include "commands/vacuum.h"
#include "hnsw.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"

/*
 * Check if deleted list contains an index tid
 */
static bool
DeletedContains(HTAB *deleted, ItemPointer indextid)
{
	bool		found;

	hash_search(deleted, indextid, HASH_FIND, &found);
	return found;
}

/*
 * Remove deleted heap tids
 *
 * OK to remove for entry point, since always considered for searches and inserts
 */
static void
RemoveHeapTids(HnswVacuumState * vacuumstate)
{
	BlockNumber blkno = HNSW_HEAD_BLKNO;
	HnswElement highestPoint = &vacuumstate->highestPoint;
	Relation	index = vacuumstate->index;
	BufferAccessStrategy bas = vacuumstate->bas;
	HnswElement entryPoint = GetEntryPoint(vacuumstate->index);

	/* Store separately since highestPoint.level is uint8 */
	int			highestLevel = -1;

	/* Initialize highest point */
	highestPoint->blkno = InvalidBlockNumber;
	highestPoint->offno = InvalidOffsetNumber;

	while (BlockNumberIsValid(blkno))
	{
		Buffer		buf;
		Page		page;
		GenericXLogState *state;
		OffsetNumber offno;
		OffsetNumber maxoffno;
		bool		updated = false;

		vacuum_delay_point();

		buf = ReadBufferExtended(index, MAIN_FORKNUM, blkno, RBM_NORMAL, bas);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		state = GenericXLogStart(index);
		page = GenericXLogRegisterBuffer(state, buf, 0);
		maxoffno = PageGetMaxOffsetNumber(page);

		/* Iterate over nodes */
		for (offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
		{
			HnswElementTuple etup = (HnswElementTuple) PageGetItem(page, PageGetItemId(page, offno));
			int			idx = 0;
			bool		itemUpdated = false;

			/* Skip neighbor tuples */
			if (!HnswIsElementTuple(etup))
				continue;

			if (ItemPointerIsValid(&etup->heaptids[0]))
			{
				for (int i = 0; i < HNSW_HEAPTIDS; i++)
				{
					/* Stop at first unused */
					if (!ItemPointerIsValid(&etup->heaptids[i]))
						break;

					if (vacuumstate->callback(&etup->heaptids[i], vacuumstate->callback_state))
						itemUpdated = true;
					else
					{
						/* Move to front of list */
						etup->heaptids[idx++] = etup->heaptids[i];
					}
				}

				if (itemUpdated)
				{
					Size		etupSize = HNSW_ELEMENT_TUPLE_SIZE(etup->vec.dim);

					/* Mark rest as invalid */
					for (int i = idx; i < HNSW_HEAPTIDS; i++)
						ItemPointerSetInvalid(&etup->heaptids[i]);

					if (!PageIndexTupleOverwrite(page, offno, (Item) etup, etupSize))
						elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));

					updated = true;
				}
			}

			if (!ItemPointerIsValid(&etup->heaptids[0]))
			{
				ItemPointerData ip;

				/* Add to deleted list */
				ItemPointerSet(&ip, blkno, offno);

				(void) hash_search(vacuumstate->deleted, &ip, HASH_ENTER, NULL);
			}
			else if (etup->level > highestLevel && !(blkno == entryPoint->blkno && offno == entryPoint->offno))
			{
				/* Keep track of highest non-entry point */
				/* TODO Keep track of closest one to entry point? */
				highestPoint->blkno = blkno;
				highestPoint->offno = offno;
				highestLevel = etup->level;
			}
		}

		blkno = HnswPageGetOpaque(page)->nextblkno;

		if (updated)
		{
			MarkBufferDirty(buf);
			GenericXLogFinish(state);
		}
		else
			GenericXLogAbort(state);

		UnlockReleaseBuffer(buf);
	}
}

/*
 * Check for deleted neighbors
 */
static bool
NeedsUpdated(HnswVacuumState * vacuumstate, HnswElement element)
{
	Relation	index = vacuumstate->index;
	BufferAccessStrategy bas = vacuumstate->bas;
	Buffer		buf;
	Page		page;
	ItemId		itemid;
	int			neighborCount;
	HnswNeighborTuple ntup;
	bool		needsUpdated = false;

	buf = ReadBufferExtended(index, MAIN_FORKNUM, element->neighborPage, RBM_NORMAL, bas);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	itemid = PageGetItemId(page, element->neighborOffno);
	ntup = (HnswNeighborTuple) PageGetItem(page, itemid);
	neighborCount = HNSW_NEIGHBOR_COUNT(itemid);

	Assert(HnswIsNeighborTuple(ntup));

	/* Check neighbors */
	for (int i = 0; i < neighborCount; i++)
	{
		HnswNeighborTupleItem *neighbor = &ntup->neighbors[i];

		if (!ItemPointerIsValid(&neighbor->indextid))
			continue;

		/* Check if in deleted list */
		if (DeletedContains(vacuumstate->deleted, &neighbor->indextid))
		{
			needsUpdated = true;
			break;
		}
	}

	UnlockReleaseBuffer(buf);

	return needsUpdated;
}

/*
 * Repair graph for a single element
 */
static void
RepairGraphElement(HnswVacuumState * vacuumstate, HnswElement element)
{
	Relation	index = vacuumstate->index;
	Buffer		buf;
	Page		page;
	GenericXLogState *state;
	int			m = vacuumstate->m;
	int			efConstruction = vacuumstate->efConstruction;
	FmgrInfo   *procinfo = vacuumstate->procinfo;
	Oid			collation = vacuumstate->collation;
	HnswElement entryPoint;
	BufferAccessStrategy bas = vacuumstate->bas;
	HnswNeighborTuple ntup = vacuumstate->ntup;
	Size		ntupSize = HNSW_NEIGHBOR_TUPLE_SIZE(element->level, m);

	/* Check if any neighbors point to deleted values */
	if (!NeedsUpdated(vacuumstate, element))
		return;

	/* Refresh entry point for each element */
	entryPoint = GetEntryPoint(index);

	/* Special case for entry point */
	if (element->blkno == entryPoint->blkno && element->offno == entryPoint->offno)
	{
		if (BlockNumberIsValid(vacuumstate->highestPoint.blkno))
		{
			/* Already updated */
			if (vacuumstate->highestPoint.blkno == element->blkno && vacuumstate->highestPoint.offno == element->offno)
				return;

			entryPoint = &vacuumstate->highestPoint;
			entryPoint->neighbors = NULL;
		}
		else
			entryPoint = NULL;
	}

	HnswInitNeighbors(element, m);
	element->heaptids = NIL;

	HnswInsertElement(element, entryPoint, index, procinfo, collation, m, efConstruction, NULL, true);

	/* Write out new neighbors on page */
	buf = ReadBufferExtended(index, MAIN_FORKNUM, element->neighborPage, RBM_NORMAL, bas);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	state = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(state, buf, 0);

	/* Update neighbors */
	HnswSetNeighborTuple(ntup, element, m);

	if (!PageIndexTupleOverwrite(page, element->neighborOffno, (Item) ntup, ntupSize))
		elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));

	/* Commit */
	MarkBufferDirty(buf);
	GenericXLogFinish(state);
	UnlockReleaseBuffer(buf);
}

/*
 * Repair graph entry point
 */
static void
RepairGraphEntryPoint(HnswVacuumState * vacuumstate)
{
	Relation	index = vacuumstate->index;
	HnswElement highestPoint = &vacuumstate->highestPoint;
	HnswElement entryPoint;
	MemoryContext oldCtx = MemoryContextSwitchTo(vacuumstate->tmpCtx);

	/* Repair graph for highest non-entry point */
	/* This may not be the highest with new inserts, but should be fine */
	if (BlockNumberIsValid(highestPoint->blkno))
	{
		HnswLoadElement(highestPoint, NULL, NULL, index, vacuumstate->procinfo, vacuumstate->collation, true);
		RepairGraphElement(vacuumstate, highestPoint);
	}

	entryPoint = GetEntryPoint(index);
	if (entryPoint != NULL)
	{
		ItemPointerData epData;

		ItemPointerSet(&epData, entryPoint->blkno, entryPoint->offno);

		if (DeletedContains(vacuumstate->deleted, &epData))
			UpdateMetaPage(index, true, highestPoint, InvalidBlockNumber, MAIN_FORKNUM);
		else
		{
			/* Highest point will be used to repair */
			HnswLoadElement(entryPoint, NULL, NULL, index, vacuumstate->procinfo, vacuumstate->collation, true);
			RepairGraphElement(vacuumstate, entryPoint);
		}
	}

	/* Reset memory context */
	MemoryContextSwitchTo(oldCtx);
	MemoryContextReset(vacuumstate->tmpCtx);
}

/*
 * Repair graph for all elements
 */
static void
RepairGraph(HnswVacuumState * vacuumstate)
{
	Relation	index = vacuumstate->index;
	BufferAccessStrategy bas = vacuumstate->bas;
	BlockNumber blkno = HNSW_HEAD_BLKNO;

	RepairGraphEntryPoint(vacuumstate);

	while (BlockNumberIsValid(blkno))
	{
		Buffer		buf;
		Page		page;
		OffsetNumber offno;
		OffsetNumber maxoffno;
		List	   *elements = NIL;
		ListCell   *lc2;
		MemoryContext oldCtx;

		vacuum_delay_point();

		oldCtx = MemoryContextSwitchTo(vacuumstate->tmpCtx);

		buf = ReadBufferExtended(index, MAIN_FORKNUM, blkno, RBM_NORMAL, bas);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		maxoffno = PageGetMaxOffsetNumber(page);

		/* Load items into memory to minimize locking */
		for (offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
		{
			HnswElementTuple etup = (HnswElementTuple) PageGetItem(page, PageGetItemId(page, offno));
			HnswElement element;

			/* Skip neighbor tuples */
			if (!HnswIsElementTuple(etup))
				continue;

			/* Skip updating neighbors if being deleted */
			if (!ItemPointerIsValid(&etup->heaptids[0]))
				continue;

			/* Create an element */
			element = palloc(sizeof(HnswElementData));
			element->neighborPage = ItemPointerGetBlockNumber(&etup->neighbortid);
			element->neighborOffno = ItemPointerGetOffsetNumber(&etup->neighbortid);
			element->level = etup->level;
			element->blkno = blkno;
			element->offno = offno;
			element->vec = palloc(VECTOR_SIZE(etup->vec.dim));
			memcpy(element->vec, &etup->vec, VECTOR_SIZE(etup->vec.dim));

			elements = lappend(elements, element);
		}

		blkno = HnswPageGetOpaque(page)->nextblkno;

		UnlockReleaseBuffer(buf);

		/* Update neighbor pages */
		foreach(lc2, elements)
			RepairGraphElement(vacuumstate, (HnswElement) lfirst(lc2));

		/* Reset memory context */
		MemoryContextSwitchTo(oldCtx);
		MemoryContextReset(vacuumstate->tmpCtx);
	}
}

/*
 * Mark items as deleted
 */
static void
MarkDeleted(HnswVacuumState * vacuumstate)
{
	BlockNumber blkno = HNSW_HEAD_BLKNO;
	BlockNumber insertPage = InvalidBlockNumber;
	Relation	index = vacuumstate->index;
	BufferAccessStrategy bas = vacuumstate->bas;
	IndexBulkDeleteResult *stats = vacuumstate->stats;

	while (BlockNumberIsValid(blkno))
	{
		Buffer		buf;
		Page		page;
		GenericXLogState *state;
		OffsetNumber offno;
		OffsetNumber maxoffno;

		vacuum_delay_point();

		buf = ReadBufferExtended(index, MAIN_FORKNUM, blkno, RBM_NORMAL, bas);

		/*
		 * ambulkdelete cannot delete entries from pages that are pinned by
		 * other backends
		 *
		 * https://www.postgresql.org/docs/current/index-locking.html
		 */
		LockBufferForCleanup(buf);

		state = GenericXLogStart(index);
		page = GenericXLogRegisterBuffer(state, buf, 0);
		maxoffno = PageGetMaxOffsetNumber(page);

		/* Update element and neighbors together */
		for (offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
		{
			HnswElementTuple etup = (HnswElementTuple) PageGetItem(page, PageGetItemId(page, offno));
			HnswNeighborTuple ntup;
			Size		etupSize;
			Size		ntupSize;
			Buffer		nbuf;
			Page		npage;
			BlockNumber neighborPage;
			OffsetNumber neighborOffno;
			int			neighborCount;

			/* Skip neighbor tuples */
			if (!HnswIsElementTuple(etup))
				continue;

			/* Skip deleted tuples */
			if (etup->deleted)
				continue;

			if (ItemPointerIsValid(&etup->heaptids[0]))
			{
				stats->num_index_tuples++;
				continue;
			}

			stats->tuples_removed++;

			/* Calculate sizes */
			etupSize = HNSW_ELEMENT_TUPLE_SIZE(etup->vec.dim);
			ntupSize = HNSW_NEIGHBOR_TUPLE_SIZE(etup->level, vacuumstate->m);

			neighborCount = (etup->level + 2) * vacuumstate->m;

			/* Get neighbor page */
			neighborPage = ItemPointerGetBlockNumber(&etup->neighbortid);
			neighborOffno = ItemPointerGetOffsetNumber(&etup->neighbortid);

			if (neighborPage == blkno)
			{
				nbuf = buf;
				npage = page;
			}
			else
			{
				nbuf = ReadBufferExtended(index, MAIN_FORKNUM, neighborPage, RBM_NORMAL, bas);
				LockBuffer(nbuf, BUFFER_LOCK_EXCLUSIVE);
				npage = GenericXLogRegisterBuffer(state, nbuf, 0);
			}

			ntup = (HnswNeighborTuple) PageGetItem(npage, PageGetItemId(npage, neighborOffno));

			/* Overwrite element */
			/* TODO Increment version? */
			etup->deleted = 1;
			MemSet(&etup->vec.x, 0, etup->vec.dim * sizeof(float));

			/* Overwrite neighbors */
			for (int i = 0; i < neighborCount; i++)
			{
				ItemPointerSetInvalid(&ntup->neighbors[i].indextid);
				ntup->neighbors[i].distance = NAN;
			}

			if (!PageIndexTupleOverwrite(page, offno, (Item) etup, etupSize))
				elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));

			if (!PageIndexTupleOverwrite(npage, neighborOffno, (Item) ntup, ntupSize))
				elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));

			/* Commit */
			MarkBufferDirty(buf);
			if (nbuf != buf)
				MarkBufferDirty(nbuf);
			GenericXLogFinish(state);
			if (nbuf != buf)
				UnlockReleaseBuffer(nbuf);

			/* Set to first free page */
			if (!BlockNumberIsValid(insertPage))
				insertPage = blkno;

			/* Prepare new xlog */
			state = GenericXLogStart(index);
			page = GenericXLogRegisterBuffer(state, buf, 0);
		}

		blkno = HnswPageGetOpaque(page)->nextblkno;

		GenericXLogAbort(state);
		UnlockReleaseBuffer(buf);
	}

	UpdateMetaPage(index, false, NULL, insertPage, MAIN_FORKNUM);
}

/*
 * Initialize the vacuum state
 */
static void
InitVacuumState(HnswVacuumState * vacuumstate, IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	index = info->index;
	HASHCTL		hash_ctl;

	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	vacuumstate->index = index;
	vacuumstate->stats = stats;
	vacuumstate->callback = callback;
	vacuumstate->callback_state = callback_state;
	vacuumstate->m = HnswGetM(index);
	vacuumstate->efConstruction = HnswGetEfConstruction(index);
	vacuumstate->bas = GetAccessStrategy(BAS_BULKREAD);
	vacuumstate->procinfo = index_getprocinfo(index, 1, HNSW_DISTANCE_PROC);
	vacuumstate->collation = index->rd_indcollation[0];
	vacuumstate->ntup = palloc0(BLCKSZ);
	vacuumstate->tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
												"Hnsw vacuum temporary context",
												ALLOCSET_DEFAULT_SIZES);

	/* Create hash table */
	hash_ctl.keysize = sizeof(ItemPointerData);
	hash_ctl.entrysize = sizeof(ItemPointerData);
	hash_ctl.hcxt = CurrentMemoryContext;
	vacuumstate->deleted = hash_create("hnswbulkdelete indextids", 256, &hash_ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/*
 * Free resources
 */
static void
FreeVacuumState(HnswVacuumState * vacuumstate)
{
	hash_destroy(vacuumstate->deleted);
	FreeAccessStrategy(vacuumstate->bas);
	pfree(vacuumstate->ntup);
	MemoryContextDelete(vacuumstate->tmpCtx);
}

/*
 * Bulk delete tuples from the index
 */
IndexBulkDeleteResult *
hnswbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			   IndexBulkDeleteCallback callback, void *callback_state)
{
	HnswVacuumState vacuumstate;

	InitVacuumState(&vacuumstate, info, stats, callback, callback_state);

	/* Pass 1: Remove heap tids */
	RemoveHeapTids(&vacuumstate);

	/* Pass 2: Repair graph */
	RepairGraph(&vacuumstate);

	/* Pass 3: Mark as deleted */
	MarkDeleted(&vacuumstate);

	FreeVacuumState(&vacuumstate);

	return vacuumstate.stats;
}

/*
 * Clean up after a VACUUM operation
 */
IndexBulkDeleteResult *
hnswvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	Relation	rel = info->index;

	if (info->analyze_only)
		return stats;

	/* stats is NULL if ambulkdelete not called */
	/* OK to return NULL if index not changed */
	if (stats == NULL)
		return NULL;

	stats->num_pages = RelationGetNumberOfBlocks(rel);

	return stats;
}
