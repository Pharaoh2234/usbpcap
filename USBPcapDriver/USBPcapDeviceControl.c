#include "USBPcapMain.h"
#include "include\USBPcap.h"
#include "USBPcapURB.h"
#include "USBPcapRootHubControl.h"
#include "USBPcapBuffer.h"

///////////////////////////////////////////////////////////////////////
// I/O device control request handlers
//
NTSTATUS DkDevCtl(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
    NTSTATUS            ntStat = STATUS_SUCCESS;
    PDEVICE_EXTENSION   pDevExt = NULL;
    PIO_STACK_LOCATION  pStack = NULL;
    ULONG               ulRes = 0, ctlCode = 0;

    pDevExt = (PDEVICE_EXTENSION) pDevObj->DeviceExtension;

    ntStat = IoAcquireRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);
    if (!NT_SUCCESS(ntStat))
    {
        DkDbgVal("Error lock!", ntStat);
        DkCompleteRequest(pIrp, ntStat, 0);
        return ntStat;
    }

    pStack = IoGetCurrentIrpStackLocation(pIrp);

    ctlCode = IoGetFunctionCodeFromCtlCode(pStack->Parameters.DeviceIoControl.IoControlCode);

    if (pDevExt->deviceMagic == USBPCAP_MAGIC_ROOTHUB ||
        pDevExt->deviceMagic == USBPCAP_MAGIC_DEVICE)
    {
        IoSkipCurrentIrpStackLocation(pIrp);
        ntStat = IoCallDriver(pDevExt->pNextDevObj, pIrp);
    }
    else if (pDevExt->deviceMagic == USBPCAP_MAGIC_CONTROL)
    {
        PDEVICE_EXTENSION      rootExt;
        PUSBPCAP_ROOTHUB_DATA  pRootData;

        rootExt = (PDEVICE_EXTENSION)pDevExt->context.control.pRootHubObject->DeviceExtension;
        pRootData = (PUSBPCAP_ROOTHUB_DATA)rootExt->context.usb.pDeviceData->pRootData;

        switch (pStack->Parameters.DeviceIoControl.IoControlCode)
        {
            case IOCTL_USBPCAP_SETUP_BUFFER:
            {
                PUSBPCAP_BUFFER_SIZE  pBufferSize;

                if (pStack->Parameters.DeviceIoControl.InputBufferLength !=
                    sizeof(USBPCAP_BUFFER_SIZE))
                {
                    ntStat = STATUS_INVALID_PARAMETER;
                    break;
                }

                pBufferSize = (PUSBPCAP_BUFFER_SIZE)pIrp->AssociatedIrp.SystemBuffer;
                DkDbgVal("IOCTL_USBPCAP_SETUP_BUFFER", pBufferSize->size);

                ntStat = USBPcapSetUpBuffer(pRootData, pBufferSize->size);
                break;
            }

            case IOCTL_USBPCAP_START_FILTERING:
                DkDbgStr("IOCTL_USBPCAP_START_FILTERING");
                pRootData->filtered = TRUE;
                break;

            case IOCTL_USBPCAP_STOP_FILTERING:
                DkDbgStr("IOCTL_USBPCAP_STOP_FILTERING");
                pRootData->filtered = FALSE;
                break;

            default:
                DkDbgVal("This: IOCTL_XXXXX", ctlCode);
                ntStat = STATUS_INVALID_DEVICE_REQUEST;
                break;
        }

        DkCompleteRequest(pIrp, ntStat, 0);
    }

    IoReleaseRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);

    return ntStat;
}


//
//--------------------------------------------------------------------------
//

////////////////////////////////////////////////////////////////////////////
// Internal I/O device control request handlers
//
NTSTATUS DkInDevCtl(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
    NTSTATUS            ntStat = STATUS_SUCCESS;
    PDEVICE_EXTENSION   pDevExt = NULL;
    PIO_STACK_LOCATION  pStack = NULL;
    PDEVICE_OBJECT      pNextDevObj = NULL;
    ULONG               ctlCode = 0;

    pDevExt = (PDEVICE_EXTENSION) pDevObj->DeviceExtension;

    ntStat = IoAcquireRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);
    if (!NT_SUCCESS(ntStat))
    {
        DkDbgVal("Error lock!", ntStat);
        DkCompleteRequest(pIrp, ntStat, 0);
        return ntStat;
    }

    pStack = IoGetCurrentIrpStackLocation(pIrp);

    ctlCode = IoGetFunctionCodeFromCtlCode(pStack->Parameters.DeviceIoControl.IoControlCode);

    if (pDevExt->deviceMagic == USBPCAP_MAGIC_ROOTHUB)
    {
        DkDbgVal("Hub Filter: IOCTL_INTERNAL_XXXXX", ctlCode);
        pNextDevObj = pDevExt->pNextDevObj;
    }
    else if (pDevExt->deviceMagic == USBPCAP_MAGIC_DEVICE)
    {
        ntStat = DkTgtInDevCtl(pDevExt, pStack, pIrp);

        IoReleaseRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);

        return ntStat;
    }
    else
    {
        DkDbgVal("This: IOCTL_INTERNAL_XXXXX", ctlCode);
        pNextDevObj = pDevExt->pNextDevObj;
    }

    IoSkipCurrentIrpStackLocation(pIrp);
    ntStat = IoCallDriver(pNextDevObj, pIrp);

    IoReleaseRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);

    return ntStat;
}


NTSTATUS DkTgtInDevCtl(PDEVICE_EXTENSION pDevExt, PIO_STACK_LOCATION pStack, PIRP pIrp)
{
    NTSTATUS            ntStat = STATUS_SUCCESS;
    PURB                pUrb = NULL;
    ULONG               ctlCode = 0;

    ntStat = IoAcquireRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);
    if (!NT_SUCCESS(ntStat))
    {
        DkDbgVal("Error lock!", ntStat);
        DkCompleteRequest(pIrp, ntStat, 0);
        return ntStat;
    }

    ctlCode = IoGetFunctionCodeFromCtlCode(pStack->Parameters.DeviceIoControl.IoControlCode);

    // Our interest is IOCTL_INTERNAL_USB_SUBMIT_URB, where USB device driver send URB to
    // it's USB bus driver
    if (pStack->Parameters.DeviceIoControl.IoControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB)
    {
        /* code here should cope with DISPATCH_LEVEL */

        // URB is collected BEFORE forward to bus driver or next lower object
        pUrb = (PURB) pStack->Parameters.Others.Argument1;
        if (pUrb != NULL)
        {
            USBPcapAnalyzeURB(pIrp, pUrb, FALSE,
                              pDevExt->context.usb.pDeviceData);
        }

        // Forward this request to bus driver or next lower object
        // with completion routine
        IoCopyCurrentIrpStackLocationToNext(pIrp);
        IoSetCompletionRoutine(pIrp,
            (PIO_COMPLETION_ROUTINE) DkTgtInDevCtlCompletion,
            NULL, TRUE, TRUE, TRUE);

        ntStat = IoCallDriver(pDevExt->pNextDevObj, pIrp);
    }
    else
    {
        DkDbgVal("IOCTL_INTERNAL_USB_XXXX", ctlCode);

        IoSkipCurrentIrpStackLocation(pIrp);
        ntStat = IoCallDriver(pDevExt->pNextDevObj, pIrp);

        IoReleaseRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);
    }

    return ntStat;
}

NTSTATUS DkTgtInDevCtlCompletion(PDEVICE_OBJECT pDevObj, PIRP pIrp, PVOID pCtx)
{
    PDEVICE_EXTENSION   pDevExt = NULL;
    PURB                pUrb = NULL;
    PIO_STACK_LOCATION  pStack = NULL;

    if (pIrp->PendingReturned)
        IoMarkIrpPending(pIrp);

    pDevExt = (PDEVICE_EXTENSION) pDevObj->DeviceExtension;

    // URB is collected AFTER forward to bus driver or next lower object
    if (NT_SUCCESS(pIrp->IoStatus.Status))
    {
        pStack = IoGetCurrentIrpStackLocation(pIrp);
        pUrb = (PURB) pStack->Parameters.Others.Argument1;
        if (pUrb != NULL)
        {
            USBPcapAnalyzeURB(pIrp, pUrb, TRUE,
                              pDevExt->context.usb.pDeviceData);
        }
        else
        {
            DkDbgStr("Bus driver returned success but the result is NULL!");
        }
    }
    else
    {
        DkDbgVal("Bus driver returned an error!", pIrp->IoStatus.Status);
    }

    IoReleaseRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);

    return STATUS_SUCCESS;
}
