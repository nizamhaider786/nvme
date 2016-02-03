/*********************************************************************************
 * Copyright 2013 VMware, Inc. All rights reserved.
 * ******************************************************************************/

/**
*******************************************************************************
** Copyright (c) 2012-2013  Integrated Device Technology, Inc.               **
**                                                                           **
** All rights reserved.                                                      **
**                                                                           **
*******************************************************************************
**                                                                           **
** Redistribution and use in source and binary forms, with or without        **
** modification, are permitted provided that the following conditions are    **
** met:                                                                      **
**                                                                           **
**   1. Redistributions of source code must retain the above copyright       **
**      notice, this list of conditions and the following disclaimer.        **
**                                                                           **
**   2. Redistributions in binary form must reproduce the above copyright    **
**      notice, this list of conditions and the following disclaimer in the  **
**      documentation and/or other materials provided with the distribution. **
**                                                                           **
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS   **
** IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, **
** THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR    **
** PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR         **
** CONTRIBUTORS BE LIABLE FOR ANY DIRECT,INDIRECT, INCIDENTAL, SPECIAL,      **
** EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,       **
** PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR        **
** PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF    **
** LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING      **
** NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS        **
** SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.              **
**                                                                           **
** The views and conclusions contained in the software and documentation     **
** are those of the authors and should not be interpreted as representing    **
** official policies, either expressed or implied,                           **
** Integrated Device Technology Inc.                                         **
**                                                                           **
*******************************************************************************
**/

/*
 * @file: nvme_queue.c --
 *
 *    Queues related functions
 */

#include "nvme_private.h"
#include "nvme_scsi_cmds.h"

#if NVME_DEBUG
#include "nvme_debug.h"
#endif /* NVME_DEBUG */



static void
Nvme_SpinlockLock(void *arg)
{
   vmk_Lock lock  = (vmk_Lock) arg;
   vmk_SpinlockLock(lock);
}

static void
Nvme_SpinlockUnlock(void *arg)
{
   vmk_Lock lock = (vmk_Lock) arg;
   vmk_SpinlockUnlock(lock);
}

static void
Nvme_GetCpu(void *arg)
{
   VMK_NOT_REACHED();
}

static void
Nvme_PutCpu(void *arg)
{
   VMK_NOT_REACHED();
}


/**
 * Acknowledge interrupt
 *
 * @param [in] handlerData queue instance
 * @param [in] intrCookie interrupt cookie
 *
 * @return VMK_OK this interrupt is for us and should schedule interrupt
 *            handler
 */
static VMK_ReturnStatus
NvmeQueue_IntrAck(void *handlerData, vmk_IntrCookie intrCookie)
{
   return VMK_OK;
}


/**
 * Interrupt handler
 *
 * Handles interrupts by processing completion queues
 *
 * @param handlerData queue instance
 * @param intrCookie interrupt cookie
 */
static void
NvmeQueue_IntrHandler(void *handlerData, vmk_IntrCookie intrCookie)
{
   struct NvmeQueueInfo *qinfo = (struct NvmeQueueInfo *)handlerData;

   qinfo->lockFunc(qinfo->lock);
   nvmeCoreProcessCq(qinfo);
   qinfo->unlockFunc(qinfo->lock);
}


/**
 * Request irq handler for a given queue
 *
 * @param [in] qinfo queue instance
 */
static VMK_ReturnStatus
NvmeQueue_RequestIrq(struct NvmeQueueInfo *qinfo)
{
   VMK_ReturnStatus vmkStatus;
   struct NvmeCtrlr *ctrlr = qinfo->ctrlr;

   if (!ctrlr->msixEnabled) {
      /* Per-queue interrupt is only available for MSIx mode */
      return VMK_BAD_PARAM;
   }
   if (qinfo->intrIndex >= ctrlr->numVectors) {
      /* Invalid interrupt index. */
      return VMK_BAD_PARAM;
   }

   vmkStatus = OsLib_IntrRegister(ctrlr->device,
      ctrlr->intrArray[qinfo->intrIndex],
      qinfo, qinfo->id, NvmeQueue_IntrAck, NvmeQueue_IntrHandler);

   return vmkStatus;
}


/**
 * Free interrupt handler for a given queue
 *
 * @param [in] qinfo queue instance
 */
static VMK_ReturnStatus
NvmeQueue_FreeIrq(struct NvmeQueueInfo *qinfo)
{
   struct NvmeCtrlr *ctrlr = qinfo->ctrlr;

   if (!ctrlr->msixEnabled) {
      /* Per-queue interrupt is only available for MSIx mode */
      return VMK_BAD_PARAM;
   }

   if (qinfo->intrIndex >= ctrlr->numVectors) {
      /* Invalid interrupt index. */
      return VMK_BAD_PARAM;
   }

   if (!NvmeCore_IsQueueSuspended(qinfo)) {
      Nvme_LogError("trying to unregister interrupts on an active queue %d.",
                    qinfo->id);
      VMK_ASSERT(0);
      return VMK_BUSY;
   }

   return OsLib_IntrUnregister(ctrlr->intrArray[qinfo->intrIndex], qinfo);
}


/**
 * @brief This function frees Command Information list.
 *    This function releases cmd information block resources.
 *    a.  free prp list
 *    b.  free sg list
 *    c.  free command information array
 *
 * @param[in] qinfo pointer to queue information block
 */
static VMK_ReturnStatus
NvmeQueue_CmdInfoDestroy(struct NvmeQueueInfo *qinfo)
{
   struct NvmeCtrlr *ctrlr = qinfo->ctrlr;
   struct NvmeCmdInfo *cmdInfo;
   int i;

   cmdInfo = qinfo->cmdList;

   if (!cmdInfo) {
      return VMK_BAD_PARAM;
   }

   for (i = 0; i < qinfo->idCount; i++) {

      if (cmdInfo->prps) {
         OsLib_DmaFree(ctrlr, &cmdInfo->dmaEntry);
         cmdInfo->prps = NULL;
         cmdInfo->prpPhy = 0L;
      } else {
         break;
      }

      cmdInfo ++;
   }

   /* By now the active cmd list should be empty */
   VMK_ASSERT(vmk_ListIsEmpty(&qinfo->cmdActive));

   /* Clear free cmd list to avoid potential allocation */
   vmk_ListInit(&qinfo->cmdFree);

   /* cmdList is freed in NvmeQueue_Destroy(). */

   return VMK_OK;
}


/**
 * @brief This function Constructs command information free list.
 *    This function creates linked list of free cmd_information
 *    block from queue's cmd_list array.
 *    a.  alloc prp list
 *    b.  alloc sg list
 *    c.  create linked list of free command information blocks
 *
 * @param[in] qinfo pointer to queue information block
 *
 * @note This function manipulates queue's free and active list,
 * caller should make sure queue is not used by any other thread.
 */
static VMK_ReturnStatus
NvmeQueue_CmdInfoConstruct(struct NvmeQueueInfo *qinfo)
{
   VMK_ReturnStatus vmkStatus;
   struct NvmeCtrlr *ctrlr;
   struct NvmeCmdInfo *cmdInfo;
#if 0
   char propName[VMK_MISC_NAME_MAX];
#endif
   int i;

   ctrlr = qinfo->ctrlr;

   vmk_ListInit(&qinfo->cmdFree);
   vmk_ListInit(&qinfo->cmdActive);

   cmdInfo = qinfo->cmdList;

   for (i = 0; i < qinfo->idCount; i++) {

      /* Use (i+1) as cid */
      cmdInfo->cmdId = i + 1;

      vmkStatus = OsLib_DmaAlloc(ctrlr, (sizeof (struct nvme_prp)) * max_prp_list,
         &cmdInfo->dmaEntry);
      if (vmkStatus != VMK_OK) {
         Nvme_LogError("Failed to allocate dma buffer.");
         goto cleanup;
      }

      cmdInfo->prps = (struct nvme_prp *)cmdInfo->dmaEntry.va;
      cmdInfo->prpPhy = cmdInfo->dmaEntry.ioa;

      /* TODO: Initialize wait queue */

      vmk_ListInsert(&cmdInfo->list, vmk_ListAtRear(&qinfo->cmdFree));

      cmdInfo ++;
   }

   return VMK_OK;

cleanup:
   /* TODO: cleanup allocated cmd ids */
   NvmeQueue_CmdInfoDestroy(qinfo);

   return vmkStatus;
}


/**
 * @brief This function Allocate Queue resources.
 *    This function allocates queue info, queue DMA buffer, Submission
 *    queue(s), its DMA buffer(s) and command information block.
 *    a. allocate queue information block.
 *    b. allocate submission queue information block
 *    c. Allocate Completion queue DMA buffer
 *    d. allocate submission queue DMA buffer
 *    e. Allocate PRP list DMA pool.
 *    f. Set Device queue information list
 *
 * Note: caller should ensure that qinfo->ctrlr is set correctly.
 *
 * @param[in] qinfo pointer to queue instance
 * @param[in] sqsize Submission queue size
 * @param[in] cqsize Completion queue size
 * @param[in] qid Queue ID
 * @param[in] shared Flag indicating this is a shared CPU queue
 * @param[in] intrIndex index of vectors into MSIx table
 *
 * @return This function returns VMK_OK if successful, otherwise Error Code
 */
VMK_ReturnStatus
NvmeQueue_Construct(struct NvmeQueueInfo *qinfo, int sqsize, int cqsize,
   int qid, int shared, int intrIndex)
{
   VMK_ReturnStatus vmkStatus;
   struct NvmeCtrlr *ctrlr = qinfo->ctrlr;
   struct NvmeSubQueueInfo *sqInfo;
   char propName[VMK_MISC_NAME_MAX];
   vmk_ByteCount size;

   if (qid >= MAX_NR_QUEUES) {
      Nvme_LogError("invalid queue id: %d.", qid);
      VMK_ASSERT(0);
      return VMK_BAD_PARAM;
   }

   qinfo->id = qid;
   qinfo->qsize = sqsize;
   qinfo->intrIndex = intrIndex;

   /* Queue init to SUSPEND state */
   qinfo->flags |= QUEUE_SUSPEND;

   /* Create a per-queue lock */
   vmk_StringFormat(propName, VMK_MISC_NAME_MAX, NULL, "nvmeCqLock-%s-%d",
                    Nvme_GetCtrlrName(ctrlr), qid);
   vmkStatus = OsLib_LockCreate(ctrlr->lockDomain, NVME_LOCK_RANK_MEDIUM,
      propName, &qinfo->lock);
   if (vmkStatus != VMK_OK) {
      return vmkStatus;
   }

   if (shared) {
      qinfo->lockFunc = Nvme_SpinlockLock;
      qinfo->unlockFunc = Nvme_SpinlockUnlock;
   } else {
      qinfo->lockFunc = Nvme_GetCpu;
      qinfo->unlockFunc = Nvme_PutCpu;
   }

   /* Now allocate submission queue info */
   sqInfo = Nvme_Alloc(sizeof(*sqInfo), 0, NVME_ALLOC_ZEROED);
   if (sqInfo == NULL) {
      vmkStatus = VMK_NO_MEMORY;
      goto free_lock;
   }

   qinfo->subQueue = sqInfo;

   sqInfo->ctrlr = ctrlr;
   sqInfo->qsize = sqsize;
   vmk_StringFormat(propName, VMK_MISC_NAME_MAX, NULL, "nvmeSqLock-%s-%d",
                    Nvme_GetCtrlrName(ctrlr), qid);
   vmkStatus = OsLib_LockCreate(ctrlr->lockDomain, NVME_LOCK_RANK_HIGH,
      propName, &sqInfo->lock);
   if (vmkStatus != VMK_OK) {
      goto free_sq;
   }

   /* Allocate completion queue DMA buffer */
   vmkStatus = OsLib_DmaAlloc(ctrlr, cqsize * sizeof(struct cq_entry), &qinfo->dmaEntry);
   if (vmkStatus != VMK_OK) {
      goto free_sq_lock;
   }
   qinfo->compq = (struct cq_entry *) qinfo->dmaEntry.va;
   qinfo->compqPhy = qinfo->dmaEntry.ioa;
   vmk_Memset(qinfo->compq, 0, cqsize * sizeof(struct cq_entry));

   /* Initialize completion head and tail */
   qinfo->head = 0;
   qinfo->tail = 0;
   qinfo->phase = 1;
   qinfo->timeoutId = -1;  /* TODO: what is this? */

   /* Allocate submission queue DMA buffer */
   vmkStatus = OsLib_DmaAlloc(ctrlr, sqsize * sizeof(struct nvme_cmd), &sqInfo->dmaEntry);
   if (vmkStatus != VMK_OK) {
      goto free_cq_dma;
   }
   sqInfo->subq = (struct nvme_cmd *) sqInfo->dmaEntry.va;
   sqInfo->subqPhy = sqInfo->dmaEntry.ioa;
   vmk_Memset(sqInfo->subq, 0, sqsize * sizeof(struct nvme_cmd));

   /*
    * Initialize Submission head and tail
    * For now, we are assuming a single submission queue
    * per completion queue. Eventually we need to allocate
    * sequential submission queue ids from pool of available ids.
    */
   sqInfo->head = 0;
   sqInfo->tail = 0;
   sqInfo->id = qid;
   sqInfo->entries = sqInfo->qsize - 1;
   ctrlr->subQueueList[qid] = sqInfo;

   /*
    * Allocate Command Information Block
    * Number of cached command IDs for IO queues are defined by
    * driver parameter "io_command_id_size".
    * Admin Queue cached commands IDs are same as queue size.
    */
   if (qid) {
      qinfo->idCount = io_command_id_size;
   } else {
      qinfo->idCount = sqsize;
   }

   size = qinfo->idCount * sizeof(* qinfo->cmdList);
   Nvme_LogDebug("Queue id: %d idCount: %d, size: %lu.",
      qid, qinfo->idCount, size);

   qinfo->cmdList = Nvme_Alloc(size, 0, NVME_ALLOC_ZEROED);
   if (qinfo->cmdList == NULL) {
      vmkStatus = VMK_NO_MEMORY;
      goto free_sq_dma;
   }

   /* TODO: Create work queues */

   sqInfo->lockFunc = qinfo->lockFunc;
   sqInfo->unlockFunc = qinfo->unlockFunc;
   sqInfo->compq = qinfo;

   ctrlr->queueList[qid] = qinfo;

   /* Set doorbell register location */
   qinfo->doorbell = ctrlr->regs + ((qid * 8) + NVME_ACQHDBL);
   sqInfo->doorbell = ctrlr->regs + ((qid * 8) + NVME_ASQTDBL);

   /* Now, we create cmd lists for this queue */
   vmkStatus = NvmeQueue_CmdInfoConstruct(qinfo);
   if (vmkStatus != VMK_OK) {
      goto free_cmd_list;
   }

   /* Lastly, if we are in MSIx mode and we are assigned a valid
    * intr cookie, register our interrupt handler */
   if (ctrlr->msixEnabled && intrIndex >= 0 && intrIndex < ctrlr->numVectors) {
      vmkStatus = NvmeQueue_RequestIrq(qinfo);
      if (vmkStatus != VMK_OK) {
         goto destroy_cmd_info;
      }
   }

   return VMK_OK;

destroy_cmd_info:
   NvmeQueue_CmdInfoDestroy(qinfo);

free_cmd_list:
   Nvme_Free(qinfo->cmdList);

free_sq_dma:
   OsLib_DmaFree(ctrlr, &sqInfo->dmaEntry);

free_cq_dma:
   OsLib_DmaFree(ctrlr, &qinfo->dmaEntry);

free_sq_lock:
   OsLib_LockDestroy(&sqInfo->lock);

free_sq:
   Nvme_Free(sqInfo);

free_lock:
   OsLib_LockDestroy(&qinfo->lock);
   return vmkStatus;
}


/**
 * @brief This function frees Queue resources.
 *    This function releases all queue info resources allocated by
 *    constructor.
 *    a. free Command Information list
 *    b. free PRP list DMA pool.
 *    c. free submission queue DMA buffer
 *    d. free completion queue DMA buffer
 *    e. free submission queue information block
 *    f. clear device queue information list
 *    g. free queue information block
 *
 * @param[in] qinfo pointer to queue information block
 */
VMK_ReturnStatus
NvmeQueue_Destroy(struct NvmeQueueInfo *qinfo)
{
   VMK_ReturnStatus vmkStatus;
   int qid;
   struct NvmeCtrlr *ctrlr = qinfo->ctrlr;
   struct NvmeSubQueueInfo *sqInfo = qinfo->subQueue;

   /*
    * Task list:
    *   - irq (if in msix mode)
    *   - cmd info
    *   - cmd list
    *   - sq dma
    *   - cq dma
    *   - sq lock
    *   - sq
    *   - cq lock
    */

   qid = qinfo->id;

   /**
    * Note: we can only destory an IO queue if there are no outstanding
    *       cmds associated with this queue.
    */
   VMK_ASSERT(qinfo->nrAct == 0);
   VMK_ASSERT(vmk_ListIsEmpty(&qinfo->cmdActive));

   if (ctrlr->msixEnabled) {
      vmkStatus = NvmeQueue_FreeIrq(qinfo);
      VMK_ASSERT(vmkStatus == VMK_OK);
   }

   vmkStatus = NvmeQueue_CmdInfoDestroy(qinfo);
   VMK_ASSERT(vmkStatus == VMK_OK);

   Nvme_Free(qinfo->cmdList);

   OsLib_DmaFree(ctrlr, &sqInfo->dmaEntry);
   sqInfo->subq = NULL;
   sqInfo->subqPhy = 0L;

   OsLib_DmaFree(ctrlr, &qinfo->dmaEntry);
   qinfo->compq = NULL;
   qinfo->compqPhy = 0L;

   OsLib_LockDestroy(&sqInfo->lock);

   Nvme_Free(sqInfo);
   qinfo->subQueue = NULL;

   OsLib_LockDestroy(&qinfo->lock);
   qinfo->lock = VMK_LOCK_INVALID;

   ctrlr->queueList[qid] = NULL;
   ctrlr->subQueueList[qid] = NULL;

   /* Note:
    *    We do not free qinfo here, since qinfo is not allocated
    *    by NvmeQueue_Construct(). The caller of NvmeQueue_Construct()
    *    shall be responsible for freeing up qinfo.
    */

   return VMK_OK;
}


#if 0
VMK_ReturnStatus
NvmeQueue_GetErrorLog(struct NvmeCtrlr *ctrlr, struct NvmeCmdInfo *cmdInfo)
{
   VMK_ReturnStatus vmkStatus;
   struct NvmeDmaEntry dmaEntry;
   vmk_uint32 param;

   vmkStatus = OsLib_DmaAlloc(ctrlr, LOG_PG_SIZE, &dmaEntry);
   if (vmkStatus != VMK_OK) {
      EPRINT("memory Allocation failure for Error Log.");
      return vmkStatus;
   }

   DPRINT("cmd_info %p, Error Completion cmdSpecific %d", cmdInfo,
                                        cmdInfo->cqEntry.param.cmdSpecific);
   param = GLP_ID_ERR_INFO;
   vmkStatus = NvmeCtrlrCmd_GetLogPage(ctrlr, &dmaEntry, VMK_FALSE);
   if (vmkStatus != VMK_OK) {
      DPRINT("Failed requesting for log page infor");
      OsLib_DmaFree(ctrlr, &dmaEntry);
   }
   return vmkStatus;

}
#endif


