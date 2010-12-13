/*      -*- linux-c -*-
 *
 * (C) Copyright IBM Corp. 2004-2008
 * (C) Copyright Pigeon Point Systems. 2010
 * (C) Copyright Nokia Siemens Networks 2010
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  This
 * file and program are licensed under a BSD style license.  See
 * the Copying file included with the OpenHPI distribution for
 * full licensing terms.
 *
 * Author(s):
 *      W. David Ashley <dashley@us.ibm.com>
 *      Renier Morales <renier@openhpi.org>
 *      Anton Pak <anton.pak@pigeonpoint.com>
 *      Ulrich Kleber <ulikleber@users.sourceforge.net>
 *
 */

#include <stddef.h>
#include <string.h>

#include <glib.h>

#include <SaHpi.h>
#include <oHpi.h>
#include <config.h>
#include <oh_domain.h>
#include <oh_error.h>
#include <marshal_hpi.h>

#include "oh_client.h"
#include "oh_client_conf.h"
#include "oh_client_session.h"


/*----------------------------------------------------------------------------*/
/* Global variables                                                           */
/*----------------------------------------------------------------------------*/

GStaticRecMutex ohc_lock = G_STATIC_REC_MUTEX_INIT;


/*----------------------------------------------------------------------------*/
/* Utility functions                                                          */
/*----------------------------------------------------------------------------*/

static SaErrorT clean_reading(SaHpiSensorReadingT *read_in,
                              SaHpiSensorReadingT *read_out)
{
    /* This is a workaround against unknown bugs in the marshal code */
    if (!read_in || !read_out) return SA_ERR_HPI_INVALID_PARAMS;

    memset(read_out, 0, sizeof(SaHpiSensorReadingT));

    read_out->IsSupported = read_in->IsSupported;

    if (read_in->IsSupported == SAHPI_TRUE) {
        if (!oh_lookup_sensorreadingtype(read_in->Type)) {
            //printf("Invalid reading type: %d\n", read_in->Type);
            return SA_ERR_HPI_INVALID_DATA;
        }
        read_out->Type = read_in->Type;
    }
    else {
        // TODO: Do we need to set dummy & reading type
        // just to keep marshalling happy?
        read_out->Type = SAHPI_SENSOR_READING_TYPE_INT64;
        read_out->Value.SensorInt64 = 0;
        return SA_OK;
    }

    if (read_in->Type == SAHPI_SENSOR_READING_TYPE_INT64) {
        read_out->Value.SensorInt64 = read_in->Value.SensorInt64;
    } else if (read_in->Type == SAHPI_SENSOR_READING_TYPE_UINT64) {
        read_out->Value.SensorUint64 = read_in->Value.SensorUint64;
    } else if (read_in->Type == SAHPI_SENSOR_READING_TYPE_FLOAT64) {
        read_out->Value.SensorFloat64 = read_in->Value.SensorFloat64;
    } else if (read_in->Type == SAHPI_SENSOR_READING_TYPE_BUFFER) {
        memcpy(read_out->Value.SensorBuffer,
               read_in->Value.SensorBuffer,
               SAHPI_SENSOR_BUFFER_LENGTH);
    }

    return SA_OK;
}

static SaErrorT clean_thresholds(SaHpiSensorThresholdsT *thrds_in,
                                 SaHpiSensorThresholdsT *thrds_out)
{
    /* This is a workaround against unknown bugs in the marshal code */
    SaErrorT rv;
    if (!thrds_in || !thrds_out) return SA_ERR_HPI_INVALID_PARAMS;

    rv = clean_reading(&thrds_in->LowCritical, &thrds_out->LowCritical);
    if (rv != SA_OK) return rv;
    rv = clean_reading(&thrds_in->LowMajor, &thrds_out->LowMajor);
    if (rv != SA_OK) return rv;
    rv = clean_reading(&thrds_in->LowMinor, &thrds_out->LowMinor);
    if (rv != SA_OK) return rv;
    rv = clean_reading(&thrds_in->UpCritical, &thrds_out->UpCritical);
    if (rv != SA_OK) return rv;
    rv = clean_reading(&thrds_in->UpMajor, &thrds_out->UpMajor);
    if (rv != SA_OK) return rv;
    rv = clean_reading(&thrds_in->UpMinor, &thrds_out->UpMinor);
    if (rv != SA_OK) return rv;
    rv = clean_reading(&thrds_in->PosThdHysteresis,
    &thrds_out->PosThdHysteresis);
    if (rv != SA_OK) return rv;
    rv = clean_reading(&thrds_in->NegThdHysteresis,
    &thrds_out->NegThdHysteresis);

    return rv;
}

static void __dehash_config(gpointer key, gpointer value, gpointer data)
{
    oHpiHandlerConfigT *hc = (oHpiHandlerConfigT *)data;

    strncpy((char *)hc->Params[hc->NumberOfParams].Name,
            (const char *)key,
            SAHPI_MAX_TEXT_BUFFER_LENGTH);
    strncpy((char *)hc->Params[hc->NumberOfParams].Value,
            (const char *)value,
            SAHPI_MAX_TEXT_BUFFER_LENGTH);

    ++hc->NumberOfParams;
}


/*----------------------------------------------------------------------------*/
/* Initialization function                                                    */
/*----------------------------------------------------------------------------*/
void oh_client_init(void)
{
    static SaHpiBoolT initialized = SAHPI_FALSE;

    if (initialized != SAHPI_FALSE) {
        return;
    }
    initialized = SAHPI_TRUE;

    // Initialize GLIB thread engine
    if (g_thread_supported() == FALSE) {
        g_thread_init(0);
    }

    oh_client_conf_init();
    ohc_sess_init();
}


/******************************************************************************/
/* HPI Client Layer                                                           */
/******************************************************************************/

/*----------------------------------------------------------------------------*/
/* saHpiVersionGet                                                            */
/*----------------------------------------------------------------------------*/

SaHpiVersionT SAHPI_API saHpiVersionGet (void)
{
    return SAHPI_INTERFACE_VERSION;
}

/*----------------------------------------------------------------------------*/
/* saHpiInitialize                                                            */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiInitialize(
    SAHPI_IN    SaHpiVersionT    RequestedVersion,
    SAHPI_IN    SaHpiUint32T     NumOptions,
    SAHPI_INOUT SaHpiInitOptionT *Options,
    SAHPI_OUT   SaHpiUint32T     *FailedOption,
    SAHPI_OUT   SaErrorT         *OptionError)
{
    if (RequestedVersion < OH_SAHPI_INTERFACE_VERSION_MIN_SUPPORTED ||
        RequestedVersion > OH_SAHPI_INTERFACE_VERSION_MAX_SUPPORTED)
    {
        return SA_ERR_HPI_UNSUPPORTED_API;
    }
    if ((NumOptions != 0) && (Options == 0)) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    // TODO implement more checks from section 5.2.1 of B.03.01 spec
    // Current implementation does not cover options check

    // TODO implement any library initialization code here
    // Current implementation does not utilize this function
    //

    oh_client_init();

    return SA_OK;
}

/*----------------------------------------------------------------------------*/
/* saHpiFinalize                                                              */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiFinalize()
{
    // TODO implement
    // TODO implement any library finalization code here
    // Current implementation does not utilize this function
    return SA_OK;
}

/*----------------------------------------------------------------------------*/
/* saHpiSessionOpen                                                           */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiSessionOpen(
    SAHPI_IN SaHpiDomainIdT   DomainId,
    SAHPI_OUT SaHpiSessionIdT *SessionId,
    SAHPI_IN  void            *SecurityParams)
{
    if (!SessionId || SecurityParams) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    if (DomainId == SAHPI_UNSPECIFIED_DOMAIN_ID) {
        DomainId = OH_DEFAULT_DOMAIN_ID;
    }

    return ohc_sess_open(DomainId, *SessionId);
}


/*----------------------------------------------------------------------------*/
/* saHpiSessionClose                                                          */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiSessionClose(
    SAHPI_IN SaHpiSessionIdT SessionId)
{
    return ohc_sess_close(SessionId);
}


/*----------------------------------------------------------------------------*/
/* saHpiDiscover                                                              */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiDiscover(
    SAHPI_IN SaHpiSessionIdT SessionId)
{
    SaErrorT rv;

    Params iparams;
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiDiscover, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiDomainInfoGet                                                         */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiDomainInfoGet(
    SAHPI_IN SaHpiSessionIdT SessionId,
    SAHPI_OUT SaHpiDomainInfoT *DomainInfo)
{
    SaErrorT rv;

    if (!DomainInfo) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams;
    Params oparams(DomainInfo);
    rv = ohc_sess_rpc(eFsaHpiDomainInfoGet, SessionId, iparams, oparams);

    /* Set Domain Id to real Domain Id */
    if (rv == SA_OK) {
        rv = ohc_sess_get_did(SessionId, DomainInfo->DomainId);
    }

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiDrtEntryGet                                                           */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiDrtEntryGet(
    SAHPI_IN SaHpiSessionIdT SessionId,
    SAHPI_IN SaHpiEntryIdT   EntryId,
    SAHPI_OUT SaHpiEntryIdT  *NextEntryId,
    SAHPI_OUT SaHpiDrtEntryT *DrtEntry)
{
    SaErrorT rv;

    if ((!DrtEntry) || (!NextEntryId) || (EntryId == SAHPI_LAST_ENTRY))
    {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&EntryId);
    Params oparams(NextEntryId, DrtEntry);
    rv = ohc_sess_rpc(eFsaHpiDrtEntryGet, SessionId, iparams, oparams);

    /* Set Domain Id to real Domain Id */
    if (rv == SA_OK) {
        rv = ohc_sess_get_did(SessionId, DrtEntry->DomainId);
    }

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiDomainTagSet                                                          */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiDomainTagSet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiTextBufferT *DomainTag)
{
    SaErrorT rv;

    if (!DomainTag) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    if (!oh_lookup_texttype(DomainTag->DataType)) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(DomainTag);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiDomainTagSet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiRptEntryGet                                                           */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiRptEntryGet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiEntryIdT    EntryId,
    SAHPI_OUT SaHpiEntryIdT   *NextEntryId,
    SAHPI_OUT SaHpiRptEntryT  *RptEntry)
{
    SaErrorT rv;

    if ((!NextEntryId) || (!RptEntry)) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    if (EntryId == SAHPI_LAST_ENTRY) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&EntryId);
    Params oparams(NextEntryId, RptEntry);
    rv = ohc_sess_rpc(eFsaHpiRptEntryGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiRptEntryGetByResourceId                                               */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiRptEntryGetByResourceId(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_OUT SaHpiRptEntryT  *RptEntry)
{
    SaErrorT rv;

    if (ResourceId == SAHPI_UNSPECIFIED_RESOURCE_ID || (!RptEntry)) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId);
    Params oparams(RptEntry);
    rv = ohc_sess_rpc(eFsaHpiRptEntryGetByResourceId, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiResourceSeveritySet                                                   */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiResourceSeveritySet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_IN SaHpiSeverityT   Severity)
{
    SaErrorT rv;

    if (ResourceId == SAHPI_UNSPECIFIED_RESOURCE_ID) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    if (!oh_lookup_severity(Severity)) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &Severity);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiResourceSeveritySet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiResourceTagSet                                                        */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiResourceTagSet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_IN SaHpiTextBufferT *ResourceTag)
{
    SaErrorT rv;

    if (!ResourceTag) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, ResourceTag);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiResourceTagSet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiMyEntityPathGet                                                       */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiMyEntityPathGet(
    SAHPI_IN  SaHpiSessionIdT  SessionId,
    SAHPI_OUT SaHpiEntityPathT *EntityPath)
{
    SaErrorT rv;

    if (!EntityPath) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams;
    Params oparams(EntityPath);
    rv = ohc_sess_rpc(eFsaHpiMyEntityPathGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiResourceIdGet                                                         */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiResourceIdGet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_OUT SaHpiResourceIdT *ResourceId)
{
    SaErrorT rv;

    if (!ResourceId) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams;
    Params oparams(ResourceId);
    rv = ohc_sess_rpc(eFsaHpiResourceIdGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiGetIdByEntityPath                                                     */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiGetIdByEntityPath(
    SAHPI_IN    SaHpiSessionIdT    SessionId,
    SAHPI_IN    SaHpiEntityPathT   EntityPath,
    SAHPI_IN    SaHpiRdrTypeT      InstrumentType,
    SAHPI_INOUT SaHpiUint32T       *InstanceId,
    SAHPI_OUT   SaHpiResourceIdT   *ResourceId,
    SAHPI_OUT   SaHpiInstrumentIdT *InstrumentId,
    SAHPI_OUT   SaHpiUint32T       *RptUpdateCount)
{
    SaErrorT rv;
    SaHpiInstrumentIdT instrument_id;

    if ((!ResourceId) || (!InstanceId) ||
        (*InstanceId == SAHPI_LAST_ENTRY) || (!RptUpdateCount) ||
        ((!InstrumentId) && (InstrumentType != SAHPI_NO_RECORD)))
    {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    if (!InstrumentId) {
        InstrumentId = &instrument_id;
    }

    Params iparams(&EntityPath, &InstrumentType, InstanceId);
    Params oparams(InstanceId, ResourceId, InstrumentId, RptUpdateCount);
    rv = ohc_sess_rpc(eFsaHpiGetIdByEntityPath, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiGetChildEntityPath                                                    */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiGetChildEntityPath(
    SAHPI_IN    SaHpiSessionIdT  SessionId,
    SAHPI_IN    SaHpiEntityPathT ParentEntityPath,
    SAHPI_INOUT SaHpiUint32T     *InstanceId,
    SAHPI_OUT   SaHpiEntityPathT *ChildEntityPath,
    SAHPI_OUT   SaHpiUint32T     *RptUpdateCount)
{
    SaErrorT rv;

    if ((!InstanceId) || (*InstanceId == SAHPI_LAST_ENTRY) || (!RptUpdateCount)) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ParentEntityPath, InstanceId);
    Params oparams(InstanceId, ChildEntityPath, RptUpdateCount);
    rv = ohc_sess_rpc(eFsaHpiGetChildEntityPath, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiResourceFailedRemove                                                  */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiResourceFailedRemove(
    SAHPI_IN    SaHpiSessionIdT        SessionId,
    SAHPI_IN    SaHpiResourceIdT       ResourceId)
{
    SaErrorT rv;

    Params iparams(&ResourceId);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiResourceFailedRemove, SessionId, iparams, oparams);

    return rv;
}

/*----------------------------------------------------------------------------*/
/* saHpiEventLogInfoGet                                                       */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiEventLogInfoGet(
    SAHPI_IN SaHpiSessionIdT     SessionId,
    SAHPI_IN SaHpiResourceIdT    ResourceId,
    SAHPI_OUT SaHpiEventLogInfoT *Info)
{
    SaErrorT rv;

    if (!Info) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId);
    Params oparams(Info);
    rv = ohc_sess_rpc(eFsaHpiEventLogInfoGet, SessionId, iparams, oparams);

    return rv;
}

/*----------------------------------------------------------------------------*/
/* saHpiEventLogCapabilitiesGet                                               */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiEventLogCapabilitiesGet(
    SAHPI_IN SaHpiSessionIdT     SessionId,
    SAHPI_IN SaHpiResourceIdT    ResourceId,
    SAHPI_OUT SaHpiEventLogCapabilitiesT  *EventLogCapabilities)
{
    SaErrorT rv;

    if (!EventLogCapabilities) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId);
    Params oparams(EventLogCapabilities);
    rv = ohc_sess_rpc(eFsaHpiEventLogCapabilitiesGet, SessionId, iparams, oparams);

    return rv;
}

/*----------------------------------------------------------------------------*/
/* saHpiEventLogEntryGet                                                      */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiEventLogEntryGet(
    SAHPI_IN SaHpiSessionIdT        SessionId,
    SAHPI_IN SaHpiResourceIdT       ResourceId,
    SAHPI_IN SaHpiEntryIdT          EntryId,
    SAHPI_OUT SaHpiEventLogEntryIdT *PrevEntryId,
    SAHPI_OUT SaHpiEventLogEntryIdT *NextEntryId,
    SAHPI_OUT SaHpiEventLogEntryT   *EventLogEntry,
    SAHPI_INOUT SaHpiRdrT           *Rdr,
    SAHPI_INOUT SaHpiRptEntryT      *RptEntry)
{
    SaErrorT rv;

    if ((!PrevEntryId) || (!EventLogEntry) || (!NextEntryId) ||
        (EntryId == SAHPI_NO_MORE_ENTRIES))
    {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    SaHpiRdrT rdr;
    SaHpiRptEntryT rpte;

    Params iparams(&ResourceId, &EntryId);
    Params oparams(PrevEntryId, NextEntryId, EventLogEntry, &rdr, &rpte);
    rv = ohc_sess_rpc(eFsaHpiEventLogEntryGet, SessionId, iparams, oparams);

    if (Rdr) {
        memcpy(Rdr, &rdr, sizeof(SaHpiRdrT));
    }
    if (RptEntry) {
        memcpy(RptEntry, &rpte, sizeof(SaHpiRptEntryT));
    }

    /* If this event is Domain Event, then adjust DomainId */
    if ((ResourceId == SAHPI_UNSPECIFIED_RESOURCE_ID) &&
        (EventLogEntry->Event.EventType == SAHPI_ET_DOMAIN))
    {
        if (rv == SA_OK) {
            SaHpiDomainIdT did;
            rv = ohc_sess_get_did(SessionId, did );
            EventLogEntry->Event.EventDataUnion.DomainEvent.DomainId = did;
        }
    }

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiEventLogEntryAdd                                                      */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiEventLogEntryAdd(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_IN SaHpiEventT      *EvtEntry)
{
    SaErrorT rv;

    if (!EvtEntry) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    if (EvtEntry->EventType != SAHPI_ET_USER ||
        EvtEntry->Source != SAHPI_UNSPECIFIED_RESOURCE_ID)
    {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    if (!oh_lookup_severity(EvtEntry->Severity)) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    if (!oh_valid_textbuffer(&EvtEntry->EventDataUnion.UserEvent.UserEventData))
    {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, EvtEntry);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiEventLogEntryAdd, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiEventLogClear                                                         */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiEventLogClear(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId)
{
    SaErrorT rv;

    Params iparams(&ResourceId);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiEventLogClear, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiEventLogTimeGet                                                       */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiEventLogTimeGet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_OUT SaHpiTimeT      *Time)
{
    SaErrorT rv;

    if (!Time) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId);
    Params oparams(Time);
    rv = ohc_sess_rpc(eFsaHpiEventLogTimeGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiEventLogTimeSet                                                       */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiEventLogTimeSet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_IN SaHpiTimeT       Time)
{
    SaErrorT rv;

    Params iparams(&ResourceId, &Time);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiEventLogTimeSet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiEventLogStateGet                                                      */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiEventLogStateGet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_OUT SaHpiBoolT      *EnableState)
{
    SaErrorT rv;

    if (!EnableState) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId);
    Params oparams(EnableState);
    rv = ohc_sess_rpc(eFsaHpiEventLogStateGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiEventLogStateSet                                                      */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiEventLogStateSet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_IN SaHpiBoolT       EnableState)
{
    SaErrorT rv;

    Params iparams(&ResourceId, &EnableState);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiEventLogStateSet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiEventLogOverflowReset                                                 */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiEventLogOverflowReset(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId)
{
    SaErrorT rv;

    Params iparams(&ResourceId);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiEventLogOverflowReset, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiSubscribe                                                             */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiSubscribe(
    SAHPI_IN SaHpiSessionIdT  SessionId)
{
    SaErrorT rv;

    Params iparams;
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiSubscribe, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiUnsSubscribe                                                          */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiUnsubscribe(
    SAHPI_IN SaHpiSessionIdT  SessionId)
{
    SaErrorT rv;

    Params iparams;
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiUnsubscribe, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiEventGet                                                              */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiEventGet(
    SAHPI_IN SaHpiSessionIdT         SessionId,
    SAHPI_IN SaHpiTimeoutT           Timeout,
    SAHPI_OUT SaHpiEventT            *Event,
    SAHPI_INOUT SaHpiRdrT            *Rdr,
    SAHPI_INOUT SaHpiRptEntryT       *RptEntry,
    SAHPI_INOUT SaHpiEvtQueueStatusT *EventQueueStatus)
{
    SaErrorT rv;

    if (Timeout < SAHPI_TIMEOUT_BLOCK || !Event) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    SaHpiRdrT rdr;
    SaHpiRptEntryT rpte;
    SaHpiEvtQueueStatusT status;

    Params iparams(&Timeout);
    Params oparams(Event, &rdr, &rpte, &status);
    rv = ohc_sess_rpc(eFsaHpiEventGet, SessionId, iparams, oparams);

    if (Rdr) {
        memcpy(Rdr, &rdr, sizeof(SaHpiRdrT));
    }
    if (RptEntry) {
        memcpy(RptEntry, &rpte, sizeof(SaHpiRptEntryT));
    }
    if (EventQueueStatus) {
        memcpy(EventQueueStatus, &status, sizeof(SaHpiEvtQueueStatusT));
    }

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiEventAdd                                                              */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiEventAdd(
    SAHPI_IN SaHpiSessionIdT SessionId,
    SAHPI_IN SaHpiEventT     *Event)
{
    SaErrorT rv;

    rv = oh_valid_addevent(Event);
    if (rv != SA_OK) return rv;

    Params iparams(Event);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiEventAdd, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiAlarmGetNext                                                          */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiAlarmGetNext(
    SAHPI_IN SaHpiSessionIdT SessionId,
    SAHPI_IN SaHpiSeverityT  Severity,
    SAHPI_IN SaHpiBoolT      Unack,
    SAHPI_INOUT SaHpiAlarmT  *Alarm)
{
    SaErrorT rv;

    if (!Alarm) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    if (!oh_lookup_severity(Severity)) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    if (Alarm->AlarmId == SAHPI_LAST_ENTRY) {
        return SA_ERR_HPI_NOT_PRESENT;
    }

    Params iparams(&Severity, &Unack, Alarm);
    Params oparams(Alarm);
    rv = ohc_sess_rpc(eFsaHpiAlarmGetNext, SessionId, iparams, oparams);

    /* Set Alarm DomainId to DomainId that HPI Application sees */
    if (rv == SA_OK) {
        rv = ohc_sess_get_did(SessionId, Alarm->AlarmCond.DomainId);
    }

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiAlarmGet                                                              */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiAlarmGet(
    SAHPI_IN SaHpiSessionIdT SessionId,
    SAHPI_IN SaHpiAlarmIdT   AlarmId,
    SAHPI_OUT SaHpiAlarmT    *Alarm)
{
    SaErrorT rv;

    if (!Alarm) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&AlarmId);
    Params oparams(Alarm);
    rv = ohc_sess_rpc(eFsaHpiAlarmGet, SessionId, iparams, oparams);

    /* Set Alarm DomainId to DomainId that HPI Application sees */
    if (rv == SA_OK) {
        rv = ohc_sess_get_did(SessionId, Alarm->AlarmCond.DomainId);
    }

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiAlarmAcknowledge                                                      */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiAlarmAcknowledge(
    SAHPI_IN SaHpiSessionIdT SessionId,
    SAHPI_IN SaHpiAlarmIdT   AlarmId,
    SAHPI_IN SaHpiSeverityT  Severity)
{
    SaErrorT rv;

    if (AlarmId == SAHPI_ENTRY_UNSPECIFIED &&
        !oh_lookup_severity(Severity))
    {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&AlarmId, &Severity);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiAlarmAcknowledge, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiAlarmAdd                                                              */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiAlarmAdd(
    SAHPI_IN SaHpiSessionIdT SessionId,
    SAHPI_INOUT SaHpiAlarmT  *Alarm)
{
    SaErrorT rv;

    if (!Alarm ||
        !oh_lookup_severity(Alarm->Severity) ||
        Alarm->AlarmCond.Type != SAHPI_STATUS_COND_TYPE_USER)
    {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(Alarm);
    Params oparams(Alarm);
    rv = ohc_sess_rpc(eFsaHpiAlarmAdd, SessionId, iparams, oparams);

    /* Set Alarm DomainId to DomainId that HPI Application sees */
    if (rv == SA_OK) {
        rv = ohc_sess_get_did(SessionId, Alarm->AlarmCond.DomainId);
    }

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiAlarmDelete                                                           */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiAlarmDelete(
    SAHPI_IN SaHpiSessionIdT SessionId,
    SAHPI_IN SaHpiAlarmIdT   AlarmId,
    SAHPI_IN SaHpiSeverityT  Severity)
{
    SaErrorT rv;

    if (AlarmId == SAHPI_ENTRY_UNSPECIFIED && !oh_lookup_severity(Severity)) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&AlarmId, &Severity);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiAlarmDelete, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiRdrGet                                                                */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiRdrGet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_IN SaHpiEntryIdT    EntryId,
    SAHPI_OUT SaHpiEntryIdT   *NextEntryId,
    SAHPI_OUT SaHpiRdrT       *Rdr)
{
    SaErrorT rv;

    if (EntryId == SAHPI_LAST_ENTRY || !Rdr || !NextEntryId) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &EntryId);
    Params oparams(NextEntryId, Rdr);
    rv = ohc_sess_rpc(eFsaHpiRdrGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiRdrGetByInstrumentId                                                  */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiRdrGetByInstrumentId(
    SAHPI_IN SaHpiSessionIdT    SessionId,
    SAHPI_IN SaHpiResourceIdT   ResourceId,
    SAHPI_IN SaHpiRdrTypeT      RdrType,
    SAHPI_IN SaHpiInstrumentIdT InstrumentId,
    SAHPI_OUT SaHpiRdrT         *Rdr)
{
    SaErrorT rv;

    if (!oh_lookup_rdrtype(RdrType) || RdrType == SAHPI_NO_RECORD || !Rdr) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &RdrType, &InstrumentId);
    Params oparams(Rdr);
    rv = ohc_sess_rpc(eFsaHpiRdrGetByInstrumentId, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiRdrUpdateCountGet                                                     */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiRdrUpdateCountGet(
    SAHPI_IN  SaHpiSessionIdT  SessionId,
    SAHPI_IN  SaHpiResourceIdT ResourceId,
    SAHPI_OUT SaHpiUint32T     *UpdateCount)
{
    SaErrorT rv;

    if (!UpdateCount) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId);
    Params oparams(UpdateCount);
    rv = ohc_sess_rpc(eFsaHpiRdrUpdateCountGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiSensorReadingGet                                                      */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiSensorReadingGet(
    SAHPI_IN SaHpiSessionIdT        SessionId,
    SAHPI_IN SaHpiResourceIdT       ResourceId,
    SAHPI_IN SaHpiSensorNumT        SensorNum,
    SAHPI_INOUT SaHpiSensorReadingT *Reading,
    SAHPI_INOUT SaHpiEventStateT    *EventState)
{
    SaErrorT rv;

    SaHpiSensorReadingT reading;
    SaHpiEventStateT state;

    Params iparams(&ResourceId, &SensorNum);
    Params oparams(&reading, &state);
    rv = ohc_sess_rpc(eFsaHpiSensorReadingGet, SessionId, iparams, oparams);

    if (Reading) {
        memcpy(Reading, &reading, sizeof(SaHpiSensorReadingT));
    }
    if (EventState) {
        memcpy(EventState, &state, sizeof(SaHpiEventStateT));
    }

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiSensorThresholdsGet                                                   */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiSensorThresholdsGet(
    SAHPI_IN SaHpiSessionIdT         SessionId,
    SAHPI_IN SaHpiResourceIdT        ResourceId,
    SAHPI_IN SaHpiSensorNumT         SensorNum,
    SAHPI_OUT SaHpiSensorThresholdsT *Thresholds)
{
    SaErrorT rv;

    if (!Thresholds) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &SensorNum);
    Params oparams(Thresholds);
    rv = ohc_sess_rpc(eFsaHpiSensorThresholdsGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiSensorThresholdsSet                                                   */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiSensorThresholdsSet(
    SAHPI_IN SaHpiSessionIdT        SessionId,
    SAHPI_IN SaHpiResourceIdT       ResourceId,
    SAHPI_IN SaHpiSensorNumT        SensorNum,
    SAHPI_IN SaHpiSensorThresholdsT *Thresholds)
{
    SaErrorT rv;
    SaHpiSensorThresholdsT tholds;

    if (!Thresholds) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    rv = clean_thresholds(Thresholds, &tholds);
    if (rv != SA_OK) return rv;

    Params iparams(&ResourceId, &SensorNum, &tholds);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiSensorThresholdsSet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiSensorTypeGet                                                         */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiSensorTypeGet(
    SAHPI_IN SaHpiSessionIdT      SessionId,
    SAHPI_IN SaHpiResourceIdT     ResourceId,
    SAHPI_IN SaHpiSensorNumT      SensorNum,
    SAHPI_OUT SaHpiSensorTypeT    *Type,
    SAHPI_OUT SaHpiEventCategoryT *Category)
{
    SaErrorT rv;

    if (!Type || !Category) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &SensorNum);
    Params oparams(Type, Category);
    rv = ohc_sess_rpc(eFsaHpiSensorTypeGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiSensorEnableGet                                                       */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiSensorEnableGet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_IN SaHpiSensorNumT  SensorNum,
    SAHPI_OUT SaHpiBoolT      *Enabled)
{
    SaErrorT rv;

    if (!Enabled) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &SensorNum);
    Params oparams(Enabled);
    rv = ohc_sess_rpc(eFsaHpiSensorEnableGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiSensorEnableSet                                                       */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiSensorEnableSet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_IN SaHpiSensorNumT  SensorNum,
    SAHPI_IN SaHpiBoolT       Enabled)
{
    SaErrorT rv;

    Params iparams(&ResourceId, &SensorNum, &Enabled);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiSensorEnableSet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiSensorEventEnableGet                                                  */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiSensorEventEnableGet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_IN SaHpiSensorNumT  SensorNum,
    SAHPI_OUT SaHpiBoolT      *Enabled)
{
    SaErrorT rv;

    if (!Enabled) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &SensorNum);
    Params oparams(Enabled);
    rv = ohc_sess_rpc(eFsaHpiSensorEventEnableGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiSensorEventEnableSet                                                  */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiSensorEventEnableSet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_IN SaHpiSensorNumT  SensorNum,
    SAHPI_IN SaHpiBoolT       Enabled)
{
    SaErrorT rv;

    Params iparams(&ResourceId, &SensorNum, &Enabled);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiSensorEventEnableSet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiSensorEventMasksGet                                                   */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiSensorEventMasksGet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_IN SaHpiSensorNumT  SensorNum,
    SAHPI_INOUT SaHpiEventStateT *Assert,
    SAHPI_INOUT SaHpiEventStateT *Deassert)
{
    SaErrorT rv;

    SaHpiEventStateT assert, deassert;

    Params iparams(&ResourceId, &SensorNum, &assert, &deassert);
    Params oparams(&assert, &deassert);
    rv = ohc_sess_rpc(eFsaHpiSensorEventMasksGet, SessionId, iparams, oparams);

    if (Assert) {
        *Assert = assert;
    }
    if (Deassert) {
        *Deassert = deassert;
    }

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiSensorEventMasksSet                                                   */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiSensorEventMasksSet(
    SAHPI_IN SaHpiSessionIdT             SessionId,
    SAHPI_IN SaHpiResourceIdT            ResourceId,
    SAHPI_IN SaHpiSensorNumT             SensorNum,
    SAHPI_IN SaHpiSensorEventMaskActionT Action,
    SAHPI_IN SaHpiEventStateT            Assert,
    SAHPI_IN SaHpiEventStateT            Deassert)
{
    SaErrorT rv;

    Params iparams(&ResourceId, &SensorNum, &Action, &Assert, &Deassert);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiSensorEventMasksSet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiControlTypeGet                                                        */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiControlTypeGet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_IN SaHpiCtrlNumT    CtrlNum,
    SAHPI_OUT SaHpiCtrlTypeT  *Type)
{
    SaErrorT rv;

    if (!Type) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &CtrlNum);
    Params oparams(Type);
    rv = ohc_sess_rpc(eFsaHpiControlTypeGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiControlGet                                                            */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiControlGet(
    SAHPI_IN SaHpiSessionIdT    SessionId,
    SAHPI_IN SaHpiResourceIdT   ResourceId,
    SAHPI_IN SaHpiCtrlNumT      CtrlNum,
    SAHPI_OUT SaHpiCtrlModeT    *Mode,
    SAHPI_INOUT SaHpiCtrlStateT *State)
{
    SaErrorT rv;

    SaHpiCtrlModeT mode;
    SaHpiCtrlStateT state;

    if (State) {
        memcpy(&state, State, sizeof(SaHpiCtrlStateT));
        if (!oh_lookup_ctrltype(state.Type)) {
            state.Type = SAHPI_CTRL_TYPE_TEXT;
        }
    }
    else {
        state.Type = SAHPI_CTRL_TYPE_TEXT;
    }

    Params iparams(&ResourceId, &CtrlNum, &state);
    Params oparams(&mode, &state);
    rv = ohc_sess_rpc(eFsaHpiControlGet, SessionId, iparams, oparams);

    if (Mode) {
        memcpy(Mode, &mode, sizeof(SaHpiCtrlModeT));
    }
    if (State) {
        memcpy(State, &state, sizeof(SaHpiCtrlStateT));
    }

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiControlSet                                                            */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiControlSet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_IN SaHpiCtrlNumT    CtrlNum,
    SAHPI_IN SaHpiCtrlModeT   Mode,
    SAHPI_IN SaHpiCtrlStateT  *State)
{
    SaErrorT rv;
    SaHpiCtrlStateT mystate, *pmystate = 0;

    if (!oh_lookup_ctrlmode(Mode) ||
        (Mode != SAHPI_CTRL_MODE_AUTO && !State) ||
        (State && State->Type == SAHPI_CTRL_TYPE_DIGITAL &&
        !oh_lookup_ctrlstatedigital(State->StateUnion.Digital)) ||
        (State && State->Type == SAHPI_CTRL_TYPE_STREAM &&
        State->StateUnion.Stream.StreamLength > SAHPI_CTRL_MAX_STREAM_LENGTH))
    {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    memset(&mystate, 0, sizeof(SaHpiCtrlStateT));
    if (Mode == SAHPI_CTRL_MODE_AUTO) {
        pmystate = &mystate;
    } else if (State && !oh_lookup_ctrltype(State->Type)) {
        return SA_ERR_HPI_INVALID_DATA;
    } else {
        pmystate = State;
    }

    Params iparams(&ResourceId, &CtrlNum, &Mode, pmystate);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiControlSet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiIdrInfoGet                                                            */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiIdrInfoGet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_IN SaHpiIdrIdT      Idrid,
    SAHPI_OUT SaHpiIdrInfoT   *Info)
{
    SaErrorT rv;

    if (!Info) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &Idrid);
    Params oparams(Info);
    rv = ohc_sess_rpc(eFsaHpiIdrInfoGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiIdrAreaHeaderGet                                                      */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiIdrAreaHeaderGet(
    SAHPI_IN SaHpiSessionIdT      SessionId,
    SAHPI_IN SaHpiResourceIdT     ResourceId,
    SAHPI_IN SaHpiIdrIdT          Idrid,
    SAHPI_IN SaHpiIdrAreaTypeT    AreaType,
    SAHPI_IN SaHpiEntryIdT        AreaId,
    SAHPI_OUT SaHpiEntryIdT       *NextAreaId,
    SAHPI_OUT SaHpiIdrAreaHeaderT *Header)
{
    SaErrorT rv;

    if (((AreaType < SAHPI_IDR_AREATYPE_INTERNAL_USE) ||
         ((AreaType > SAHPI_IDR_AREATYPE_PRODUCT_INFO) &&
         (AreaType != SAHPI_IDR_AREATYPE_UNSPECIFIED)  &&
         (AreaType != SAHPI_IDR_AREATYPE_OEM)) ||
         (AreaId == SAHPI_LAST_ENTRY)||
         (!NextAreaId) ||
         (!Header)))
    {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &Idrid, &AreaType, &AreaId);
    Params oparams(NextAreaId, Header);
    rv = ohc_sess_rpc(eFsaHpiIdrAreaHeaderGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiIdrAreaAdd                                                            */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiIdrAreaAdd(
    SAHPI_IN SaHpiSessionIdT   SessionId,
    SAHPI_IN SaHpiResourceIdT  ResourceId,
    SAHPI_IN SaHpiIdrIdT       Idrid,
    SAHPI_IN SaHpiIdrAreaTypeT AreaType,
    SAHPI_OUT SaHpiEntryIdT    *AreaId)
{
    SaErrorT rv;

    if (!oh_lookup_idrareatype(AreaType) || (!AreaId) ) {
        return SA_ERR_HPI_INVALID_PARAMS;
    } else if (AreaType == SAHPI_IDR_AREATYPE_UNSPECIFIED) {
        return SA_ERR_HPI_INVALID_DATA;
    }

    Params iparams(&ResourceId, &Idrid, &AreaType);
    Params oparams(AreaId);
    rv = ohc_sess_rpc(eFsaHpiIdrAreaAdd, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiIdrAreaAddById                                                        */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiIdrAreaAddById(
    SAHPI_IN SaHpiSessionIdT   SessionId,
    SAHPI_IN SaHpiResourceIdT  ResourceId,
    SAHPI_IN SaHpiIdrIdT       Idrid,
    SAHPI_IN SaHpiIdrAreaTypeT AreaType,
    SAHPI_IN SaHpiEntryIdT     AreaId)
{
    SaErrorT rv;

    if (!oh_lookup_idrareatype(AreaType))   {
        return SA_ERR_HPI_INVALID_PARAMS;
    } else if (AreaType == SAHPI_IDR_AREATYPE_UNSPECIFIED) {
        return SA_ERR_HPI_INVALID_DATA;
    }

    Params iparams(&ResourceId, &Idrid, &AreaType, &AreaId);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiIdrAreaAddById, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiIdrAreaDelete                                                         */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiIdrAreaDelete(
    SAHPI_IN SaHpiSessionIdT   SessionId,
    SAHPI_IN SaHpiResourceIdT  ResourceId,
    SAHPI_IN SaHpiIdrIdT       Idrid,
    SAHPI_IN SaHpiEntryIdT     AreaId)
{
    SaErrorT rv;

    if (AreaId == SAHPI_LAST_ENTRY) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &Idrid, &AreaId);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiIdrAreaDelete, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiIdrFieldGet                                                           */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiIdrFieldGet(
    SAHPI_IN SaHpiSessionIdT    SessionId,
    SAHPI_IN SaHpiResourceIdT   ResourceId,
    SAHPI_IN SaHpiIdrIdT        Idrid,
    SAHPI_IN SaHpiEntryIdT      AreaId,
    SAHPI_IN SaHpiIdrFieldTypeT FieldType,
    SAHPI_IN SaHpiEntryIdT      FieldId,
    SAHPI_OUT SaHpiEntryIdT     *NextId,
    SAHPI_OUT SaHpiIdrFieldT    *Field)
{
    SaErrorT rv;

    if (!Field ||
        !oh_lookup_idrfieldtype(FieldType) ||
        AreaId == SAHPI_LAST_ENTRY ||
        FieldId == SAHPI_LAST_ENTRY ||
        !NextId)
    {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &Idrid, &AreaId, &FieldType, &FieldId);
    Params oparams(NextId, Field);
    rv = ohc_sess_rpc(eFsaHpiIdrFieldGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiIdrFieldAdd                                                           */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiIdrFieldAdd(
    SAHPI_IN SaHpiSessionIdT    SessionId,
    SAHPI_IN SaHpiResourceIdT   ResourceId,
    SAHPI_IN SaHpiIdrIdT        Idrid,
    SAHPI_INOUT SaHpiIdrFieldT  *Field)
{
    SaErrorT rv;

    if (!Field)   {
        return SA_ERR_HPI_INVALID_PARAMS;
    } else if (!oh_lookup_idrfieldtype(Field->Type)) {
        return SA_ERR_HPI_INVALID_PARAMS;
    } else if (Field->Type == SAHPI_IDR_FIELDTYPE_UNSPECIFIED) {
        return SA_ERR_HPI_INVALID_PARAMS;
    } else if (oh_valid_textbuffer(&Field->Field) != SAHPI_TRUE) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &Idrid, Field);
    Params oparams(Field);
    rv = ohc_sess_rpc(eFsaHpiIdrFieldAdd, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiIdrFieldAddById                                                       */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiIdrFieldAddById(
    SAHPI_IN SaHpiSessionIdT    SessionId,
    SAHPI_IN SaHpiResourceIdT   ResourceId,
    SAHPI_IN SaHpiIdrIdT        Idrid,
    SAHPI_INOUT SaHpiIdrFieldT  *Field)
{
    SaErrorT rv;

    if (!Field)   {
        return SA_ERR_HPI_INVALID_PARAMS;
    } else if (!oh_lookup_idrfieldtype(Field->Type)) {
        return SA_ERR_HPI_INVALID_PARAMS;
    } else if (Field->Type == SAHPI_IDR_FIELDTYPE_UNSPECIFIED) {
        return SA_ERR_HPI_INVALID_PARAMS;
    } else if (oh_valid_textbuffer(&Field->Field) != SAHPI_TRUE) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &Idrid, Field);
    Params oparams(Field);
    rv = ohc_sess_rpc(eFsaHpiIdrFieldAddById, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiIdrFieldSet                                                           */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiIdrFieldSet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_IN SaHpiIdrIdT      Idrid,
    SAHPI_IN SaHpiIdrFieldT   *Field)
{
    SaErrorT rv;

    if (!Field) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    if (Field->Type > SAHPI_IDR_FIELDTYPE_CUSTOM) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &Idrid, Field);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiIdrFieldSet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiIdrFieldDelete                                                        */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiIdrFieldDelete(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_IN SaHpiIdrIdT      Idrid,
    SAHPI_IN SaHpiEntryIdT    AreaId,
    SAHPI_IN SaHpiEntryIdT    FieldId)
{
    SaErrorT rv;

    if (FieldId == SAHPI_LAST_ENTRY || AreaId == SAHPI_LAST_ENTRY) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &Idrid, &AreaId, &FieldId);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiIdrFieldDelete, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiWatchdogTimerGet                                                      */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiWatchdogTimerGet(
    SAHPI_IN SaHpiSessionIdT   SessionId,
    SAHPI_IN SaHpiResourceIdT  ResourceId,
    SAHPI_IN SaHpiWatchdogNumT WatchdogNum,
    SAHPI_OUT SaHpiWatchdogT   *Watchdog)
{
    SaErrorT rv;

    if (!Watchdog) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &WatchdogNum);
    Params oparams(Watchdog);
    rv = ohc_sess_rpc(eFsaHpiWatchdogTimerGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiWatchdogTimerSet                                                      */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiWatchdogTimerSet(
    SAHPI_IN SaHpiSessionIdT   SessionId,
    SAHPI_IN SaHpiResourceIdT  ResourceId,
    SAHPI_IN SaHpiWatchdogNumT WatchdogNum,
    SAHPI_IN SaHpiWatchdogT    *Watchdog)
{
    SaErrorT rv;

    if (!Watchdog ||
        !oh_lookup_watchdogtimeruse(Watchdog->TimerUse) ||
        !oh_lookup_watchdogaction(Watchdog->TimerAction) ||
        !oh_lookup_watchdogpretimerinterrupt(Watchdog->PretimerInterrupt))
    {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    if (Watchdog->PreTimeoutInterval > Watchdog->InitialCount) {
        return SA_ERR_HPI_INVALID_DATA;
    }

    Params iparams(&ResourceId, &WatchdogNum, Watchdog);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiWatchdogTimerSet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiWatchdogTimerReset                                                    */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiWatchdogTimerReset(
    SAHPI_IN SaHpiSessionIdT   SessionId,
    SAHPI_IN SaHpiResourceIdT  ResourceId,
    SAHPI_IN SaHpiWatchdogNumT WatchdogNum)
{
    SaErrorT rv;

    Params iparams(&ResourceId, &WatchdogNum);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiWatchdogTimerReset, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiAnnunciatorGetNext                                                    */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiAnnunciatorGetNext(
    SAHPI_IN SaHpiSessionIdT       SessionId,
    SAHPI_IN SaHpiResourceIdT      ResourceId,
    SAHPI_IN SaHpiAnnunciatorNumT  AnnNum,
    SAHPI_IN SaHpiSeverityT        Severity,
    SAHPI_IN SaHpiBoolT            Unack,
    SAHPI_INOUT SaHpiAnnouncementT *Announcement)
{
    SaErrorT rv;

    if (!Announcement) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    if (!oh_lookup_severity(Severity)) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &AnnNum, &Severity, &Unack, Announcement);
    Params oparams(Announcement);
    rv = ohc_sess_rpc(eFsaHpiAnnunciatorGetNext, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiAnnunciatorGet                                                        */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiAnnunciatorGet(
    SAHPI_IN SaHpiSessionIdT      SessionId,
    SAHPI_IN SaHpiResourceIdT     ResourceId,
    SAHPI_IN SaHpiAnnunciatorNumT AnnNum,
    SAHPI_IN SaHpiEntryIdT        EntryId,
    SAHPI_OUT SaHpiAnnouncementT  *Announcement)
{
    SaErrorT rv;

    if (!Announcement) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &AnnNum, &EntryId);
    Params oparams(Announcement);
    rv = ohc_sess_rpc(eFsaHpiAnnunciatorGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiAnnunciatorAcknowledge                                                */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiAnnunciatorAcknowledge(
    SAHPI_IN SaHpiSessionIdT      SessionId,
    SAHPI_IN SaHpiResourceIdT     ResourceId,
    SAHPI_IN SaHpiAnnunciatorNumT AnnNum,
    SAHPI_IN SaHpiEntryIdT        EntryId,
    SAHPI_IN SaHpiSeverityT       Severity)
{
    SaErrorT rv;

    SaHpiSeverityT sev = SAHPI_DEBUG;

    if (EntryId == SAHPI_ENTRY_UNSPECIFIED) {
        if (!oh_lookup_severity(Severity)) {
            return SA_ERR_HPI_INVALID_PARAMS;
        } else {
            sev = Severity;
        }
    }

    Params iparams(&ResourceId, &AnnNum, &EntryId, &sev);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiAnnunciatorAcknowledge, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiAnnunciatorAdd                                                        */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiAnnunciatorAdd(
    SAHPI_IN SaHpiSessionIdT       SessionId,
    SAHPI_IN SaHpiResourceIdT      ResourceId,
    SAHPI_IN SaHpiAnnunciatorNumT  AnnNum,
    SAHPI_INOUT SaHpiAnnouncementT *Announcement)
{
    SaErrorT rv;

    if (!Announcement) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    if (Announcement->Severity == SAHPI_ALL_SEVERITIES ||
        !oh_lookup_severity(Announcement->Severity) ||
        !oh_valid_textbuffer(&Announcement->StatusCond.Data) ||
        !oh_lookup_statuscondtype(Announcement->StatusCond.Type))
    {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &AnnNum, Announcement);
    Params oparams(Announcement);
    rv = ohc_sess_rpc(eFsaHpiAnnunciatorAdd, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiAnnunciatorDelete                                                     */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiAnnunciatorDelete(
    SAHPI_IN SaHpiSessionIdT      SessionId,
    SAHPI_IN SaHpiResourceIdT     ResourceId,
    SAHPI_IN SaHpiAnnunciatorNumT AnnNum,
    SAHPI_IN SaHpiEntryIdT        EntryId,
    SAHPI_IN SaHpiSeverityT       Severity)
{
    SaErrorT rv;

    SaHpiSeverityT sev = SAHPI_DEBUG;

    if (EntryId == SAHPI_ENTRY_UNSPECIFIED) {
        if (!oh_lookup_severity(Severity)) {
            printf("Bad severity %d passed in.\n", Severity);
            return SA_ERR_HPI_INVALID_PARAMS;
        } else {
            sev = Severity;
        }
    }

    Params iparams(&ResourceId, &AnnNum, &EntryId, &sev);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiAnnunciatorDelete, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiAnnunciatorModeGet                                                    */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiAnnunciatorModeGet(
    SAHPI_IN SaHpiSessionIdT        SessionId,
    SAHPI_IN SaHpiResourceIdT       ResourceId,
    SAHPI_IN SaHpiAnnunciatorNumT   AnnNum,
    SAHPI_OUT SaHpiAnnunciatorModeT *Mode)
{
    SaErrorT rv;

    if (!Mode) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &AnnNum);
    Params oparams(Mode);
    rv = ohc_sess_rpc(eFsaHpiAnnunciatorModeGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiAnnunciatorModeSet                                                    */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiAnnunciatorModeSet(
    SAHPI_IN SaHpiSessionIdT       SessionId,
    SAHPI_IN SaHpiResourceIdT      ResourceId,
    SAHPI_IN SaHpiAnnunciatorNumT  AnnNum,
    SAHPI_IN SaHpiAnnunciatorModeT Mode)
{
    SaErrorT rv;

    if (!oh_lookup_annunciatormode(Mode)) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &AnnNum, &Mode);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiAnnunciatorModeSet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiDimiInfoGet                                                           */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiDimiInfoGet(
    SAHPI_IN SaHpiSessionIdT   SessionId,
    SAHPI_IN SaHpiResourceIdT  ResourceId,
    SAHPI_IN SaHpiDimiNumT      DimiNum,
    SAHPI_OUT SaHpiDimiInfoT   *DimiInfo)
{
    SaErrorT rv;

    if (!DimiInfo) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &DimiNum);
    Params oparams(DimiInfo);
    rv = ohc_sess_rpc(eFsaHpiDimiInfoGet, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiDimiTestInfoGet(
    SAHPI_IN    SaHpiSessionIdT      SessionId,
    SAHPI_IN    SaHpiResourceIdT     ResourceId,
    SAHPI_IN    SaHpiDimiNumT        DimiNum,
    SAHPI_IN    SaHpiDimiTestNumT    TestNum,
    SAHPI_OUT   SaHpiDimiTestT       *DimiTest)
{
    SaErrorT rv;

    if (!DimiTest) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &DimiNum, &TestNum);
    Params oparams(DimiTest);
    rv = ohc_sess_rpc(eFsaHpiDimiTestInfoGet, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiDimiTestReadinessGet(
    SAHPI_IN    SaHpiSessionIdT      SessionId,
    SAHPI_IN    SaHpiResourceIdT     ResourceId,
    SAHPI_IN    SaHpiDimiNumT        DimiNum,
    SAHPI_IN    SaHpiDimiTestNumT    TestNum,
    SAHPI_OUT   SaHpiDimiReadyT      *DimiReady)
{
    SaErrorT rv;

    if (!DimiReady) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &DimiNum, &TestNum);
    Params oparams(DimiReady);
    rv = ohc_sess_rpc(eFsaHpiDimiTestReadinessGet, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiDimiTestStart(
    SAHPI_IN  SaHpiSessionIdT                SessionId,
    SAHPI_IN  SaHpiResourceIdT               ResourceId,
    SAHPI_IN  SaHpiDimiNumT                  DimiNum,
    SAHPI_IN  SaHpiDimiTestNumT              TestNum,
    SAHPI_IN  SaHpiUint8T                    NumberOfParams,
    SAHPI_IN  SaHpiDimiTestVariableParamsT   *ParamsList)
{
    SaErrorT rv;
    SaHpiDimiTestVariableParamsListT params_list;

    if ((!ParamsList) && (NumberOfParams != 0)) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    params_list.NumberOfParams = NumberOfParams;
    params_list.ParamsList = ParamsList;

    Params iparams(&ResourceId, &DimiNum, &TestNum, &params_list);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiDimiTestStart, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiDimiTestCancel(
    SAHPI_IN    SaHpiSessionIdT      SessionId,
    SAHPI_IN    SaHpiResourceIdT     ResourceId,
    SAHPI_IN    SaHpiDimiNumT        DimiNum,
    SAHPI_IN    SaHpiDimiTestNumT    TestNum)
{
    SaErrorT rv;

    Params iparams(&ResourceId, &DimiNum, &TestNum);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiDimiTestCancel, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiDimiTestStatusGet(
    SAHPI_IN    SaHpiSessionIdT                   SessionId,
    SAHPI_IN    SaHpiResourceIdT                  ResourceId,
    SAHPI_IN    SaHpiDimiNumT                     DimiNum,
    SAHPI_IN    SaHpiDimiTestNumT                 TestNum,
    SAHPI_OUT   SaHpiDimiTestPercentCompletedT    *PercentCompleted,
    SAHPI_OUT   SaHpiDimiTestRunStatusT           *RunStatus)
{
    SaErrorT rv;
    SaHpiDimiTestPercentCompletedT percent;
    SaHpiDimiTestPercentCompletedT *ppercent = &percent;

    if (!RunStatus) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    if (!PercentCompleted) {
        ppercent = PercentCompleted;
    }

    Params iparams(&ResourceId, &DimiNum, &TestNum);
    Params oparams(ppercent, RunStatus);
    rv = ohc_sess_rpc(eFsaHpiDimiTestStatusGet, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiDimiTestResultsGet(
    SAHPI_IN    SaHpiSessionIdT          SessionId,
    SAHPI_IN    SaHpiResourceIdT         ResourceId,
    SAHPI_IN    SaHpiDimiNumT            DimiNum,
    SAHPI_IN    SaHpiDimiTestNumT        TestNum,
    SAHPI_OUT   SaHpiDimiTestResultsT    *TestResults)
{
    SaErrorT rv;

    if (!TestResults) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &DimiNum, &TestNum);
    Params oparams(TestResults);
    rv = ohc_sess_rpc(eFsaHpiDimiTestResultsGet, SessionId, iparams, oparams);

    return rv;
}

/*******************************************************************************
 *
 * FUMI Functions
 *
 ******************************************************************************/

SaErrorT SAHPI_API saHpiFumiSpecInfoGet(
    SAHPI_IN    SaHpiSessionIdT    SessionId,
    SAHPI_IN    SaHpiResourceIdT   ResourceId,
    SAHPI_IN    SaHpiFumiNumT      FumiNum,
    SAHPI_OUT   SaHpiFumiSpecInfoT *SpecInfo)
{
    SaErrorT rv;

    if (!SpecInfo) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &FumiNum);
    Params oparams(SpecInfo);
    rv = ohc_sess_rpc(eFsaHpiFumiSpecInfoGet, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiFumiServiceImpactGet(
    SAHPI_IN   SaHpiSessionIdT              SessionId,
    SAHPI_IN   SaHpiResourceIdT             ResourceId,
    SAHPI_IN   SaHpiFumiNumT                FumiNum,
    SAHPI_OUT  SaHpiFumiServiceImpactDataT  *ServiceImpact)
{
    SaErrorT rv;

    if (!ServiceImpact) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &FumiNum);
    Params oparams(ServiceImpact);
    rv = ohc_sess_rpc(eFsaHpiFumiServiceImpactGet, SessionId, iparams, oparams);

    return rv;
}



SaErrorT SAHPI_API saHpiFumiSourceSet (
    SAHPI_IN    SaHpiSessionIdT         SessionId,
    SAHPI_IN    SaHpiResourceIdT        ResourceId,
    SAHPI_IN    SaHpiFumiNumT           FumiNum,
    SAHPI_IN    SaHpiBankNumT           BankNum,
    SAHPI_IN    SaHpiTextBufferT        *SourceUri)
{
    SaErrorT rv;

    if ((!SourceUri) || SourceUri->DataType != SAHPI_TL_TYPE_TEXT) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &FumiNum, &BankNum, SourceUri);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiFumiSourceSet, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiFumiSourceInfoValidateStart (
    SAHPI_IN    SaHpiSessionIdT       SessionId,
    SAHPI_IN    SaHpiResourceIdT      ResourceId,
    SAHPI_IN    SaHpiFumiNumT         FumiNum,
    SAHPI_IN    SaHpiBankNumT         BankNum)
{
    SaErrorT rv;

    Params iparams(&ResourceId, &FumiNum, &BankNum);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiFumiSourceInfoValidateStart, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiFumiSourceInfoGet (
    SAHPI_IN    SaHpiSessionIdT       SessionId,
    SAHPI_IN    SaHpiResourceIdT      ResourceId,
    SAHPI_IN    SaHpiFumiNumT         FumiNum,
    SAHPI_IN    SaHpiBankNumT         BankNum,
    SAHPI_OUT   SaHpiFumiSourceInfoT  *SourceInfo)
{
    SaErrorT rv;

    if (!SourceInfo) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &FumiNum, &BankNum);
    Params oparams(SourceInfo);
    rv = ohc_sess_rpc(eFsaHpiFumiSourceInfoGet, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiFumiSourceComponentInfoGet(
    SAHPI_IN    SaHpiSessionIdT         SessionId,
    SAHPI_IN    SaHpiResourceIdT        ResourceId,
    SAHPI_IN    SaHpiFumiNumT           FumiNum,
    SAHPI_IN    SaHpiBankNumT           BankNum,
    SAHPI_IN    SaHpiEntryIdT           ComponentEntryId,
    SAHPI_OUT   SaHpiEntryIdT           *NextComponentEntryId,
    SAHPI_OUT   SaHpiFumiComponentInfoT *ComponentInfo)
{
    SaErrorT rv;

    if ((!NextComponentEntryId) || (!ComponentInfo)) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    if (ComponentEntryId == SAHPI_LAST_ENTRY) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &FumiNum, &BankNum, &ComponentEntryId);
    Params oparams(NextComponentEntryId, ComponentInfo);
    rv = ohc_sess_rpc(eFsaHpiFumiSourceComponentInfoGet, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiFumiTargetInfoGet (
    SAHPI_IN    SaHpiSessionIdT       SessionId,
    SAHPI_IN    SaHpiResourceIdT      ResourceId,
    SAHPI_IN    SaHpiFumiNumT         FumiNum,
    SAHPI_IN    SaHpiBankNumT         BankNum,
    SAHPI_OUT   SaHpiFumiBankInfoT    *BankInfo)
{
    SaErrorT rv;

    if (!BankInfo) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &FumiNum, &BankNum);
    Params oparams(BankInfo);
    rv = ohc_sess_rpc(eFsaHpiFumiTargetInfoGet, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiFumiTargetComponentInfoGet(
    SAHPI_IN    SaHpiSessionIdT         SessionId,
    SAHPI_IN    SaHpiResourceIdT        ResourceId,
    SAHPI_IN    SaHpiFumiNumT           FumiNum,
    SAHPI_IN    SaHpiBankNumT           BankNum,
    SAHPI_IN    SaHpiEntryIdT           ComponentEntryId,
    SAHPI_OUT   SaHpiEntryIdT           *NextComponentEntryId,
    SAHPI_OUT   SaHpiFumiComponentInfoT *ComponentInfo)
{
    SaErrorT rv;

    if ((!NextComponentEntryId) || (!ComponentInfo)) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    if (ComponentEntryId == SAHPI_LAST_ENTRY) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &FumiNum, &BankNum, &ComponentEntryId);
    Params oparams(NextComponentEntryId, ComponentInfo);
    rv = ohc_sess_rpc(eFsaHpiFumiTargetComponentInfoGet, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiFumiLogicalTargetInfoGet(
    SAHPI_IN    SaHpiSessionIdT             SessionId,
    SAHPI_IN    SaHpiResourceIdT            ResourceId,
    SAHPI_IN    SaHpiFumiNumT               FumiNum,
    SAHPI_OUT   SaHpiFumiLogicalBankInfoT   *BankInfo)
{
    SaErrorT rv;

    if (!BankInfo) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &FumiNum);
    Params oparams(BankInfo);
    rv = ohc_sess_rpc(eFsaHpiFumiLogicalTargetInfoGet, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiFumiLogicalTargetComponentInfoGet(
    SAHPI_IN    SaHpiSessionIdT                SessionId,
    SAHPI_IN    SaHpiResourceIdT               ResourceId,
    SAHPI_IN    SaHpiFumiNumT                  FumiNum,
    SAHPI_IN    SaHpiEntryIdT                  ComponentEntryId,
    SAHPI_OUT   SaHpiEntryIdT                  *NextComponentEntryId,
    SAHPI_OUT   SaHpiFumiLogicalComponentInfoT *ComponentInfo)
{
    SaErrorT rv;

    if ((!NextComponentEntryId) || (!ComponentInfo)) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    if (ComponentEntryId == SAHPI_LAST_ENTRY) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &FumiNum, &ComponentEntryId);
    Params oparams(NextComponentEntryId, ComponentInfo);
    rv = ohc_sess_rpc(eFsaHpiFumiLogicalTargetComponentInfoGet, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiFumiBackupStart(
    SAHPI_IN    SaHpiSessionIdT       SessionId,
    SAHPI_IN    SaHpiResourceIdT      ResourceId,
    SAHPI_IN    SaHpiFumiNumT         FumiNum)
{
    SaErrorT rv;

    Params iparams(&ResourceId, &FumiNum);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiFumiBackupStart, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiFumiBankBootOrderSet (
    SAHPI_IN    SaHpiSessionIdT       SessionId,
    SAHPI_IN    SaHpiResourceIdT      ResourceId,
    SAHPI_IN    SaHpiFumiNumT         FumiNum,
    SAHPI_IN    SaHpiBankNumT         BankNum,
    SAHPI_IN    SaHpiUint32T          Position)
{
    SaErrorT rv;

    Params iparams(&ResourceId, &FumiNum, &BankNum, &Position);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiFumiBankBootOrderSet, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiFumiBankCopyStart(
    SAHPI_IN    SaHpiSessionIdT       SessionId,
    SAHPI_IN    SaHpiResourceIdT      ResourceId,
    SAHPI_IN    SaHpiFumiNumT         FumiNum,
    SAHPI_IN    SaHpiBankNumT         SourceBankNum,
    SAHPI_IN    SaHpiBankNumT         TargetBankNum)
{
    SaErrorT rv;

    Params iparams(&ResourceId, &FumiNum, &SourceBankNum, &TargetBankNum);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiFumiBankCopyStart, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiFumiInstallStart (
    SAHPI_IN    SaHpiSessionIdT       SessionId,
    SAHPI_IN    SaHpiResourceIdT      ResourceId,
    SAHPI_IN    SaHpiFumiNumT         FumiNum,
    SAHPI_IN    SaHpiBankNumT         BankNum)
{
    SaErrorT rv;

    Params iparams(&ResourceId, &FumiNum, &BankNum);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiFumiInstallStart, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiFumiUpgradeStatusGet (
    SAHPI_IN    SaHpiSessionIdT         SessionId,
    SAHPI_IN    SaHpiResourceIdT        ResourceId,
    SAHPI_IN    SaHpiFumiNumT           FumiNum,
    SAHPI_IN    SaHpiBankNumT           BankNum,
    SAHPI_OUT   SaHpiFumiUpgradeStatusT *UpgradeStatus)
{
    SaErrorT rv;

    if (!UpgradeStatus) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &FumiNum, &BankNum);
    Params oparams(UpgradeStatus);
    rv = ohc_sess_rpc(eFsaHpiFumiUpgradeStatusGet, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiFumiTargetVerifyStart (
    SAHPI_IN    SaHpiSessionIdT       SessionId,
    SAHPI_IN    SaHpiResourceIdT      ResourceId,
    SAHPI_IN    SaHpiFumiNumT         FumiNum,
    SAHPI_IN    SaHpiBankNumT         BankNum)
{
    SaErrorT rv;

    Params iparams(&ResourceId, &FumiNum, &BankNum);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiFumiTargetVerifyStart, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiFumiTargetVerifyMainStart(
    SAHPI_IN    SaHpiSessionIdT  SessionId,
    SAHPI_IN    SaHpiResourceIdT ResourceId,
    SAHPI_IN    SaHpiFumiNumT    FumiNum)
{
    SaErrorT rv;

    Params iparams(&ResourceId, &FumiNum);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiFumiTargetVerifyMainStart, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiFumiUpgradeCancel (
    SAHPI_IN    SaHpiSessionIdT       SessionId,
    SAHPI_IN    SaHpiResourceIdT      ResourceId,
    SAHPI_IN    SaHpiFumiNumT         FumiNum,
    SAHPI_IN    SaHpiBankNumT         BankNum)
{
    SaErrorT rv;

    Params iparams(&ResourceId, &FumiNum, &BankNum);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiFumiUpgradeCancel, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiFumiAutoRollbackDisableGet(
    SAHPI_IN    SaHpiSessionIdT  SessionId,
    SAHPI_IN    SaHpiResourceIdT ResourceId,
    SAHPI_IN    SaHpiFumiNumT    FumiNum,
    SAHPI_OUT   SaHpiBoolT       *Disable)
{
    SaErrorT rv;

    if (!Disable) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &FumiNum);
    Params oparams(Disable);
    rv = ohc_sess_rpc(eFsaHpiFumiAutoRollbackDisableGet, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiFumiAutoRollbackDisableSet(
    SAHPI_IN    SaHpiSessionIdT  SessionId,
    SAHPI_IN    SaHpiResourceIdT ResourceId,
    SAHPI_IN    SaHpiFumiNumT    FumiNum,
    SAHPI_IN    SaHpiBoolT       Disable)
{
    SaErrorT rv;

    Params iparams(&ResourceId, &FumiNum, &Disable);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiFumiAutoRollbackDisableSet, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiFumiRollbackStart (
    SAHPI_IN    SaHpiSessionIdT       SessionId,
    SAHPI_IN    SaHpiResourceIdT      ResourceId,
    SAHPI_IN    SaHpiFumiNumT         FumiNum)
{
    SaErrorT rv;

    Params iparams(&ResourceId, &FumiNum);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiFumiRollbackStart, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiFumiActivate (
    SAHPI_IN    SaHpiSessionIdT       SessionId,
    SAHPI_IN    SaHpiResourceIdT      ResourceId,
    SAHPI_IN    SaHpiFumiNumT         FumiNum)
{
    SaErrorT rv;

    Params iparams(&ResourceId, &FumiNum);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiFumiActivate, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiFumiActivateStart(
    SAHPI_IN    SaHpiSessionIdT  SessionId,
    SAHPI_IN    SaHpiResourceIdT ResourceId,
    SAHPI_IN    SaHpiFumiNumT    FumiNum,
    SAHPI_IN    SaHpiBoolT       Logical)
{
    SaErrorT rv;

    Params iparams(&ResourceId, &FumiNum, &Logical);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiFumiActivateStart, SessionId, iparams, oparams);

    return rv;
}

SaErrorT SAHPI_API saHpiFumiCleanup(
    SAHPI_IN    SaHpiSessionIdT  SessionId,
    SAHPI_IN    SaHpiResourceIdT ResourceId,
    SAHPI_IN    SaHpiFumiNumT    FumiNum,
    SAHPI_IN    SaHpiBankNumT    BankNum)
{
    SaErrorT rv;

    Params iparams(&ResourceId, &FumiNum, &BankNum);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiFumiCleanup, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiHotSwapPolicyCancel                                                   */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiHotSwapPolicyCancel(
    SAHPI_IN SaHpiSessionIdT       SessionId,
    SAHPI_IN SaHpiResourceIdT      ResourceId)
{
    SaErrorT rv;

    Params iparams(&ResourceId);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiHotSwapPolicyCancel, SessionId, iparams, oparams);

    return rv;
}

/*----------------------------------------------------------------------------*/
/* saHpiResourceActiveSet                                                     */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiResourceActiveSet(
    SAHPI_IN SaHpiSessionIdT       SessionId,
    SAHPI_IN SaHpiResourceIdT      ResourceId)
{
    SaErrorT rv;

    Params iparams(&ResourceId);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiResourceActiveSet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiResourceInactiveSet                                                   */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiResourceInactiveSet(
    SAHPI_IN SaHpiSessionIdT       SessionId,
    SAHPI_IN SaHpiResourceIdT      ResourceId)
{
    SaErrorT rv;

    Params iparams(&ResourceId);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiResourceInactiveSet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiAutoInsertTimeoutGet                                                  */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiAutoInsertTimeoutGet(
    SAHPI_IN SaHpiSessionIdT SessionId,
    SAHPI_OUT SaHpiTimeoutT  *Timeout)
{
    SaErrorT rv;

    if (!Timeout) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams;
    Params oparams(Timeout);
    rv = ohc_sess_rpc(eFsaHpiAutoInsertTimeoutGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiAutoInsertTimeoutSet                                                  */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiAutoInsertTimeoutSet(
    SAHPI_IN SaHpiSessionIdT SessionId,
    SAHPI_IN SaHpiTimeoutT   Timeout)
{
    SaErrorT rv;

    if (Timeout != SAHPI_TIMEOUT_IMMEDIATE &&
        Timeout != SAHPI_TIMEOUT_BLOCK &&
        Timeout < 0)
    {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&Timeout);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiAutoInsertTimeoutSet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiAutoExtractGet                                                        */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiAutoExtractTimeoutGet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_OUT SaHpiTimeoutT   *Timeout)
{
    SaErrorT rv;

    if (!Timeout) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId);
    Params oparams(Timeout);
    rv = ohc_sess_rpc(eFsaHpiAutoExtractTimeoutGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiAutoExtractTimeoutSet                                                 */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiAutoExtractTimeoutSet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_IN SaHpiTimeoutT    Timeout)
{
    SaErrorT rv;

    if (Timeout != SAHPI_TIMEOUT_IMMEDIATE &&
        Timeout != SAHPI_TIMEOUT_BLOCK &&
        Timeout < 0)
    {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &Timeout);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiAutoExtractTimeoutSet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiHotSwapStateGet                                                       */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiHotSwapStateGet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_OUT SaHpiHsStateT   *State)
{
    SaErrorT rv;

    if (!State) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId);
    Params oparams(State);
    rv = ohc_sess_rpc(eFsaHpiHotSwapStateGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiHotSwapActionRequest                                                  */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiHotSwapActionRequest(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_IN SaHpiHsActionT   Action)
{
    SaErrorT rv;

    if (!oh_lookup_hsaction(Action)) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &Action);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiHotSwapActionRequest, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiHotSwapIndicatorStateGet                                              */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiHotSwapIndicatorStateGet(
    SAHPI_IN SaHpiSessionIdT         SessionId,
    SAHPI_IN SaHpiResourceIdT        ResourceId,
    SAHPI_OUT SaHpiHsIndicatorStateT *State)
{
    SaErrorT rv;

    if (!State) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId);
    Params oparams(State);
    rv = ohc_sess_rpc(eFsaHpiHotSwapIndicatorStateGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiHotSwapIndicatorStateSet                                              */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiHotSwapIndicatorStateSet(
    SAHPI_IN SaHpiSessionIdT        SessionId,
    SAHPI_IN SaHpiResourceIdT       ResourceId,
    SAHPI_IN SaHpiHsIndicatorStateT State)
{
    SaErrorT rv;

    if (!oh_lookup_hsindicatorstate(State)) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &State);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiHotSwapIndicatorStateSet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiParmControl                                                           */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiParmControl(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_IN SaHpiParmActionT Action)
{
    SaErrorT rv;

    if (!oh_lookup_parmaction(Action)) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &Action);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiParmControl, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiResourceLoadIdGet                                                     */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiResourceLoadIdGet(
    SAHPI_IN  SaHpiSessionIdT  SessionId,
    SAHPI_IN  SaHpiResourceIdT ResourceId,
    SAHPI_OUT SaHpiLoadIdT *LoadId)
{
    SaErrorT rv;

    Params iparams(&ResourceId);
    Params oparams(LoadId);
    rv = ohc_sess_rpc(eFsaHpiResourceLoadIdGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiResourceLoadIdSet                                                     */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiResourceLoadIdSet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_IN SaHpiLoadIdT *LoadId)
{
    SaErrorT rv;

    if (!LoadId) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, LoadId);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiResourceLoadIdSet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiResourceResetStateGet                                                 */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiResourceResetStateGet(
    SAHPI_IN SaHpiSessionIdT    SessionId,
    SAHPI_IN SaHpiResourceIdT   ResourceId,
    SAHPI_OUT SaHpiResetActionT *Action)
{
    SaErrorT rv;

    if (!Action) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId);
    Params oparams(Action);
    rv = ohc_sess_rpc(eFsaHpiResourceResetStateGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiResourceResetStateSet                                                 */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiResourceResetStateSet(
    SAHPI_IN SaHpiSessionIdT   SessionId,
    SAHPI_IN SaHpiResourceIdT  ResourceId,
    SAHPI_IN SaHpiResetActionT Action)
{
    SaErrorT rv;

    if (!oh_lookup_resetaction(Action)) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &Action);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiResourceResetStateSet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiResourcePowerStateGet                                                 */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiResourcePowerStateGet(
    SAHPI_IN SaHpiSessionIdT   SessionId,
    SAHPI_IN SaHpiResourceIdT  ResourceId,
    SAHPI_OUT SaHpiPowerStateT *State)
{
    SaErrorT rv;

    if (!State) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId);
    Params oparams(State);
    rv = ohc_sess_rpc(eFsaHpiResourcePowerStateGet, SessionId, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* saHpiResourcePowerStateSet                                                 */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API saHpiResourcePowerStateSet(
    SAHPI_IN SaHpiSessionIdT  SessionId,
    SAHPI_IN SaHpiResourceIdT ResourceId,
    SAHPI_IN SaHpiPowerStateT State)
{
    SaErrorT rv;

    if (!oh_lookup_powerstate(State)) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&ResourceId, &State);
    Params oparams;
    rv = ohc_sess_rpc(eFsaHpiResourcePowerStateSet, SessionId, iparams, oparams);

    return rv;
}

/*----------------------------------------------------------------------------*/
/* oHpiVersionGet                                                             */
/*----------------------------------------------------------------------------*/

SaHpiUint64T SAHPI_API oHpiVersionGet(void)
{
    SaHpiUint64T v = 0;

    OHPI_VERSION_GET(v, VERSION);

    return v;
}

/*----------------------------------------------------------------------------*/
/* oHpiHandlerCreate                                                          */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API oHpiHandlerCreate (
    SAHPI_IN    SaHpiSessionIdT sid,
    SAHPI_IN    GHashTable *config,
    SAHPI_OUT   oHpiHandlerIdT *id)
{
    SaErrorT rv;
    oHpiHandlerConfigT handler_config;

    if (!config || !id) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    if (g_hash_table_size(config) == 0) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    handler_config.Params = g_new0(oHpiHandlerConfigParamT, g_hash_table_size(config));
    handler_config.NumberOfParams = 0;
    // add each hash table entry to the marshable handler_config
    g_hash_table_foreach(config, __dehash_config, &handler_config);

    // now create the handler
    Params iparams(&handler_config);
    Params oparams(id);
    rv = ohc_sess_rpc(eFoHpiHandlerCreate, sid, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* oHpiHandlerDestroy                                                         */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API oHpiHandlerDestroy (
    SAHPI_IN    SaHpiSessionIdT sid,
    SAHPI_IN    oHpiHandlerIdT id)
{
    SaErrorT rv;

    if (id == 0) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&id);
    Params oparams;
    rv = ohc_sess_rpc(eFoHpiHandlerDestroy, sid, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* oHpiHandlerInfo                                                            */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API oHpiHandlerInfo (
    SAHPI_IN    SaHpiSessionIdT sid,
    SAHPI_IN    oHpiHandlerIdT id,
    SAHPI_OUT   oHpiHandlerInfoT *info,
    SAHPI_INOUT GHashTable *conf_params)
{
    SaErrorT rv;
    oHpiHandlerConfigT config;

    if (id == 0 || !info || !conf_params) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    if (g_hash_table_size(conf_params) != 0) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&id);
    Params oparams(info, &config);
    rv = ohc_sess_rpc(eFoHpiHandlerInfo, sid, iparams, oparams);

    for (unsigned int n = 0; n < config.NumberOfParams; n++) {
        g_hash_table_insert(conf_params,
                            g_strdup((const gchar *)config.Params[n].Name),
                            g_strdup((const gchar *)config.Params[n].Value));
    }
    g_free(config.Params);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* oHpiHandlerGetNext                                                         */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API oHpiHandlerGetNext (
    SAHPI_IN    SaHpiSessionIdT sid,
    SAHPI_IN    oHpiHandlerIdT id,
    SAHPI_OUT   oHpiHandlerIdT *next_id)
{
    SaErrorT rv;

    if (!next_id) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&id);
    Params oparams(next_id);
    rv = ohc_sess_rpc(eFoHpiHandlerGetNext, sid, iparams, oparams);

    return rv;
}

/*----------------------------------------------------------------------------*/
/* oHpiHandlerFind                                                            */
/*----------------------------------------------------------------------------*/
SaErrorT SAHPI_API oHpiHandlerFind (
    SAHPI_IN    SaHpiSessionIdT sid,
    SAHPI_IN    SaHpiResourceIdT rid,
    SAHPI_OUT   oHpiHandlerIdT *id)
{
    SaErrorT rv;

    if (!id || !rid) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    *id = 0; //Initialize output var

    Params iparams(&sid, &rid);
    Params oparams(id);
    rv = ohc_sess_rpc(eFoHpiHandlerFind, sid, iparams, oparams);

    return rv;
}

/*----------------------------------------------------------------------------*/
/* oHpiHandlerRetry                                                           */
/*----------------------------------------------------------------------------*/
SaErrorT SAHPI_API oHpiHandlerRetry (
    SAHPI_IN    SaHpiSessionIdT sid,
    SAHPI_IN    oHpiHandlerIdT id)
{
    SaErrorT rv;

    if (id == 0) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&id);
    Params oparams;
    rv = ohc_sess_rpc(eFoHpiHandlerRetry, sid, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* oHpiGlobalParamGet                                                         */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API oHpiGlobalParamGet (
    SAHPI_IN    SaHpiSessionIdT sid,
    SAHPI_OUT   oHpiGlobalParamT *param)
{
    SaErrorT rv;

    if (!param) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(param);
    Params oparams(param);
    rv = ohc_sess_rpc(eFoHpiGlobalParamGet, sid, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* oHpiGlobalParamSet                                                         */
/*----------------------------------------------------------------------------*/

SaErrorT SAHPI_API oHpiGlobalParamSet (
    SAHPI_IN    SaHpiSessionIdT sid,
    SAHPI_IN    oHpiGlobalParamT *param)
{
    SaErrorT rv;

    if (!param) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(param);
    Params oparams(param);
    rv = ohc_sess_rpc(eFoHpiGlobalParamSet, sid, iparams, oparams);

    return rv;
}


/*----------------------------------------------------------------------------*/
/* oHpiInjectEvent                                                            */
/*----------------------------------------------------------------------------*/
SaErrorT SAHPI_API oHpiInjectEvent (
    SAHPI_IN    SaHpiSessionIdT sid,
    SAHPI_IN    oHpiHandlerIdT id,
    SAHPI_IN    SaHpiEventT    *event,
    SAHPI_IN    SaHpiRptEntryT *rpte,
    SAHPI_IN    SaHpiRdrT *rdr)
{
    SaErrorT rv;

    if (id == 0 || !event || !rpte || !rdr) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }

    Params iparams(&id, event, rpte, rdr);
    Params oparams(&id, event, rpte, rdr);
    rv = ohc_sess_rpc(eFoHpiInjectEvent, sid, iparams, oparams);

    return rv;
}



/*----------------------------------------------------------------------------*/
/* oHpiDomainAdd                                                              */
/*----------------------------------------------------------------------------*/
SaErrorT SAHPI_API oHpiDomainAdd (
    SAHPI_IN    const SaHpiTextBufferT *host,
    SAHPI_IN    SaHpiUint16T port,
    SAHPI_OUT   SaHpiDomainIdT *domain_id)
{
    if (!host) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    if (!domain_id) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    if ((host->DataType != SAHPI_TL_TYPE_BCDPLUS) &&
         (host->DataType != SAHPI_TL_TYPE_ASCII6) &&
         (host->DataType != SAHPI_TL_TYPE_TEXT))
    {
        return SA_ERR_HPI_INVALID_DATA;
    }

    // Function may be called before first session was opened,
    // so we may need to initialize
    oh_client_init();

    char buf[SAHPI_MAX_TEXT_BUFFER_LENGTH+1];
    memcpy(&buf[0], &host->Data[0], host->DataLength);
    buf[host->DataLength] = '\0';

    return oh_add_domain_conf(buf, port, domain_id);
}



/*----------------------------------------------------------------------------*/
/* oHpiDomainAddById                                                          */
/*----------------------------------------------------------------------------*/
SaErrorT SAHPI_API oHpiDomainAddById (
    SAHPI_IN    SaHpiDomainIdT domain_id,
    SAHPI_IN    const SaHpiTextBufferT *host,
    SAHPI_IN    SaHpiUint16T port)
{
    if (!host) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    if ((host->DataType != SAHPI_TL_TYPE_BCDPLUS) &&
         (host->DataType != SAHPI_TL_TYPE_ASCII6) &&
         (host->DataType != SAHPI_TL_TYPE_TEXT))
    {
        return SA_ERR_HPI_INVALID_DATA;
    }

    // Function may be called before first session was opened,
    // so we may need to initialize
    oh_client_init();

    char buf[SAHPI_MAX_TEXT_BUFFER_LENGTH+1];
    memcpy(&buf[0], &host->Data[0], host->DataLength);
    buf[host->DataLength] = '\0';

    return oh_add_domain_conf_by_id(domain_id, buf, port);
}

/*----------------------------------------------------------------------------*/
/* oHpiDomainEntryGet                                                         */
/*----------------------------------------------------------------------------*/
SaErrorT SAHPI_API oHpiDomainEntryGet (
    SAHPI_IN    SaHpiEntryIdT    EntryId,
    SAHPI_OUT   SaHpiEntryIdT    *NextEntryId,
    SAHPI_OUT   oHpiDomainEntryT *DomainEntry)

{
    if (!NextEntryId || !DomainEntry) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    // Function may be called before first session was opened,
    // so we may need to initialize
    oh_client_init();

    const oh_domain_conf *dc = oh_get_next_domain_conf(EntryId, NextEntryId);
    if (dc == 0) { // no config for did found
        return SA_ERR_HPI_NOT_PRESENT;
    }

    DomainEntry->id = (SaHpiDomainIdT) EntryId;
    if (oh_init_textbuffer(&DomainEntry->daemonhost) != SA_OK) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    if (oh_append_textbuffer(&DomainEntry->daemonhost, dc->host)!= SA_OK) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    DomainEntry->port = dc->port;

    return SA_OK;
}


/*----------------------------------------------------------------------------*/
/* oHpiDomainEntryGetByDomainId                                               */
/*----------------------------------------------------------------------------*/
SaErrorT SAHPI_API oHpiDomainEntryGetByDomainId (
    SAHPI_IN    SaHpiDomainIdT    DomainId,
    SAHPI_OUT   oHpiDomainEntryT *DomainEntry)

{
    if (!DomainEntry) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    // Function may be called before first session was opened,
    // so we may need to initialize
    oh_client_init();

    const oh_domain_conf *entry = oh_get_domain_conf (DomainId);
    if (entry == 0) { // no config for did found
        return SA_ERR_HPI_NOT_PRESENT;
    }

    DomainEntry->id = DomainId;
    if (oh_init_textbuffer(&DomainEntry->daemonhost) != SA_OK) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    if (oh_append_textbuffer(&DomainEntry->daemonhost, entry->host)!= SA_OK) {
        return SA_ERR_HPI_INVALID_PARAMS;
    }
    DomainEntry->port = entry->port;

    return SA_OK;
}

