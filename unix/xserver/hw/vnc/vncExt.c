/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright 2011-2015 Pierre Ossman for Cendio AB
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#define NEED_EVENTS
#include "misc.h"
#include "os.h"
#include "dixstruct.h"
#include "extnsionst.h"
#include "scrnintstr.h"
#include "selection.h"

#define _VNCEXT_SERVER_
#define _VNCEXT_PROTO_
#include "vncExt.h"

#include "xorg-version.h"

#include "vncExtInit.h"
#include "RFBGlue.h"

static int ProcVncExtDispatch(ClientPtr client);
static int SProcVncExtDispatch(ClientPtr client);
static void vncResetProc(ExtensionEntry* extEntry);

static void vncClientStateChange(CallbackListPtr*, void *, void *);

static void vncSelectionCallback(CallbackListPtr *callbacks,
                                 void * data, void * args);

static int vncErrorBase = 0;
static int vncEventBase = 0;

int vncNoClipboard = 0;

static char* clientCutText = NULL;
static int clientCutTextLen = 0;

static struct VncInputSelect* vncInputSelectHead = NULL;

struct VncInputSelect {
  ClientPtr client;
  Window window;
  int mask;
  struct VncInputSelect* next;
};

int vncAddExtension(void)
{
  ExtensionEntry* extEntry;

  extEntry = AddExtension(VNCEXTNAME, VncExtNumberEvents, VncExtNumberErrors,
                          ProcVncExtDispatch, SProcVncExtDispatch, vncResetProc,
                          StandardMinorOpcode);
  if (!extEntry) {
    ErrorF("vncAddExtension: AddExtension failed\n");
    return -1;
  }

  vncErrorBase = extEntry->errorBase;
  vncEventBase = extEntry->eventBase;

  if (!AddCallback(&ClientStateCallback, vncClientStateChange, 0)) {
    FatalError("Add ClientStateCallback failed\n");
  }

  if (!AddCallback(&SelectionCallback, vncSelectionCallback, 0)) {
    FatalError("Add SelectionCallback failed\n");
  }

  return 0;
}

int vncNotifyQueryConnect(void)
{
  int count;
  xVncExtQueryConnectNotifyEvent ev;

  ev.type = vncEventBase + VncExtQueryConnectNotify;

  count = 0;
  for (struct VncInputSelect* cur = vncInputSelectHead; cur; cur = cur->next) {
    if (cur->mask & VncExtQueryConnectMask) {
      ev.sequenceNumber = cur->client->sequence;
      ev.window = cur->window;
      if (cur->client->swapped) {
#if XORG < 112
        int n;
        swaps(&ev.sequenceNumber, n);
        swapl(&ev.window, n);
#else
        swaps(&ev.sequenceNumber);
        swapl(&ev.window);
#endif
      }
      WriteToClient(cur->client, sizeof(xVncExtQueryConnectNotifyEvent),
                    (char *)&ev);
      count++;
    }
  }

  return count;
}

void vncClientCutText(const char* str, int len)
{
  xVncExtClientCutTextNotifyEvent ev;

  if (clientCutText != NULL)
    free(clientCutText);
  clientCutTextLen = 0;

  clientCutText = malloc(len);
  if (clientCutText == NULL) {
    ErrorF("Could not allocate clipboard buffer\n");
    return;
  }

  memcpy(clientCutText, str, len);
  clientCutTextLen = len;

  ev.type = vncEventBase + VncExtClientCutTextNotify;
  for (struct VncInputSelect* cur = vncInputSelectHead; cur; cur = cur->next) {
    if (cur->mask & VncExtClientCutTextMask) {
      ev.sequenceNumber = cur->client->sequence;
      ev.window = cur->window;
      ev.time = GetTimeInMillis();
      if (cur->client->swapped) {
#if XORG < 112
        int n;
        swaps(&ev.sequenceNumber, n);
        swapl(&ev.window, n);
        swapl(&ev.time, n);
#else
        swaps(&ev.sequenceNumber);
        swapl(&ev.window);
        swapl(&ev.time);
#endif
      }
      WriteToClient(cur->client, sizeof(xVncExtClientCutTextNotifyEvent),
                    (char *)&ev);
    }
  }
}

static int ProcVncExtSetParam(ClientPtr client)
{
  char *param;
  xVncExtSetParamReply rep;

  REQUEST(xVncExtSetParamReq);
  REQUEST_FIXED_SIZE(xVncExtSetParamReq, stuff->paramLen);

  param = malloc(stuff->paramLen+1);
  if (param == NULL)
    return BadAlloc;
  strncpy(param, (char*)&stuff[1], stuff->paramLen);
  param[stuff->paramLen] = '\0';

  rep.type = X_Reply;
  rep.length = 0;
  rep.success = 0;
  rep.sequenceNumber = client->sequence;

  /*
   * Prevent change of clipboard related parameters if clipboard is disabled.
   */
  if (vncNoClipboard &&
      (strncasecmp(param, "SendCutText", 11) == 0 ||
       strncasecmp(param, "AcceptCutText", 13) == 0))
    goto deny;

  if (!vncOverrideParam(param))
    goto deny;

  rep.success = 1;

  // Send DesktopName update if desktop name has been changed
  if (strncasecmp(param, "desktop", 7) == 0)
    vncUpdateDesktopName();

deny:
  free(param);

  if (client->swapped) {
#if XORG < 112
    int n;
    swaps(&rep.sequenceNumber, n);
    swapl(&rep.length, n);
#else
    swaps(&rep.sequenceNumber);
    swapl(&rep.length);
#endif
  }
  WriteToClient(client, sizeof(xVncExtSetParamReply), (char *)&rep);
  return (client->noClientException);
}

static int SProcVncExtSetParam(ClientPtr client)
{
  REQUEST(xVncExtSetParamReq);
#if XORG < 112
  register char n;
  swaps(&stuff->length, n);
#else
  swaps(&stuff->length);
#endif
  REQUEST_AT_LEAST_SIZE(xVncExtSetParamReq);
  return ProcVncExtSetParam(client);
}

static int ProcVncExtGetParam(ClientPtr client)
{
  char* param;
  char* value;
  size_t len;
  xVncExtGetParamReply rep;

  REQUEST(xVncExtGetParamReq);
  REQUEST_FIXED_SIZE(xVncExtGetParamReq, stuff->paramLen);

  param = malloc(stuff->paramLen+1);
  if (param == NULL)
    return BadAlloc;
  strncpy(param, (char*)&stuff[1], stuff->paramLen);
  param[stuff->paramLen] = 0;

  value = vncGetParam(param);
  len = value ? strlen(value) : 0;

  free(param);

  rep.type = X_Reply;
  rep.sequenceNumber = client->sequence;
  rep.success = 0;
  if (value)
    rep.success = 1;
  rep.length = (len + 3) >> 2;
  rep.valueLen = len;
  if (client->swapped) {
#if XORG < 112
    int n;
    swaps(&rep.sequenceNumber, n);
    swapl(&rep.length, n);
    swaps(&rep.valueLen, n);
#else
    swaps(&rep.sequenceNumber);
    swapl(&rep.length);
    swaps(&rep.valueLen);
#endif
  }
  WriteToClient(client, sizeof(xVncExtGetParamReply), (char *)&rep);
  if (value)
    WriteToClient(client, len, value);
  free(value);
  return (client->noClientException);
}

static int SProcVncExtGetParam(ClientPtr client)
{
  REQUEST(xVncExtGetParamReq);
#if XORG < 112
  register char n;
  swaps(&stuff->length, n);
#else
  swaps(&stuff->length);
#endif
  REQUEST_AT_LEAST_SIZE(xVncExtGetParamReq);
  return ProcVncExtGetParam(client);
}

static int ProcVncExtGetParamDesc(ClientPtr client)
{
  char* param;
  const char* desc;
  size_t len;
  xVncExtGetParamDescReply rep;

  REQUEST(xVncExtGetParamDescReq);
  REQUEST_FIXED_SIZE(xVncExtGetParamDescReq, stuff->paramLen);

  param = malloc(stuff->paramLen+1);
  if (param == NULL)
    return BadAlloc;
  strncpy(param, (char*)&stuff[1], stuff->paramLen);
  param[stuff->paramLen] = 0;

  desc = vncGetParamDesc(param);
  len = desc ? strlen(desc) : 0;

  free(param);

  rep.type = X_Reply;
  rep.sequenceNumber = client->sequence;
  rep.success = 0;
  if (desc)
    rep.success = 1;
  rep.length = (len + 3) >> 2;
  rep.descLen = len;
  if (client->swapped) {
#if XORG < 112
    int n;
    swaps(&rep.sequenceNumber, n);
    swapl(&rep.length, n);
    swaps(&rep.descLen, n);
#else
    swaps(&rep.sequenceNumber);
    swapl(&rep.length);
    swaps(&rep.descLen);
#endif
  }
  WriteToClient(client, sizeof(xVncExtGetParamDescReply), (char *)&rep);
  if (desc)
    WriteToClient(client, len, desc);
  return (client->noClientException);
}

static int SProcVncExtGetParamDesc(ClientPtr client)
{
  REQUEST(xVncExtGetParamDescReq);
#if XORG < 112
  register char n;
  swaps(&stuff->length, n);
#else
  swaps(&stuff->length);
#endif
  REQUEST_AT_LEAST_SIZE(xVncExtGetParamDescReq);
  return ProcVncExtGetParamDesc(client);
}

static int ProcVncExtListParams(ClientPtr client)
{
  xVncExtListParamsReply rep;
  char *params;
  size_t len;

  REQUEST_SIZE_MATCH(xVncExtListParamsReq);

  rep.type = X_Reply;
  rep.sequenceNumber = client->sequence;

  params = vncGetParamList();
  if (params == NULL)
    return BadAlloc;

  len = strlen(params);

  rep.length = (len + 3) >> 2;
  rep.nParams = vncGetParamCount();
  if (client->swapped) {
#if XORG < 112
    int n;
    swaps(&rep.sequenceNumber, n);
    swapl(&rep.length, n);
    swaps(&rep.nParams, n);
#else
    swaps(&rep.sequenceNumber);
    swapl(&rep.length);
    swaps(&rep.nParams);
#endif
  }
  WriteToClient(client, sizeof(xVncExtListParamsReply), (char *)&rep);
  WriteToClient(client, len, (char*)params);
  free(params);
  return (client->noClientException);
}

static int SProcVncExtListParams(ClientPtr client)
{
  REQUEST(xVncExtListParamsReq);
#if XORG < 112
  register char n;
  swaps(&stuff->length, n);
#else
  swaps(&stuff->length);
#endif
  REQUEST_SIZE_MATCH(xVncExtListParamsReq);
  return ProcVncExtListParams(client);
}

static int ProcVncExtSetServerCutText(ClientPtr client)
{
  REQUEST(xVncExtSetServerCutTextReq);
  REQUEST_FIXED_SIZE(xVncExtSetServerCutTextReq, stuff->textLen);
  vncServerCutText((const char*)&stuff[1], stuff->textLen);
  return (client->noClientException);
}

static int SProcVncExtSetServerCutText(ClientPtr client)
{
  REQUEST(xVncExtSetServerCutTextReq);
#if XORG < 112
  register char n;
  swaps(&stuff->length, n);
#else
  swaps(&stuff->length);
#endif
  REQUEST_AT_LEAST_SIZE(xVncExtSetServerCutTextReq);
#if XORG < 112
  swapl(&stuff->textLen, n);
#else
  swapl(&stuff->textLen);
#endif
  return ProcVncExtSetServerCutText(client);
}

static int ProcVncExtGetClientCutText(ClientPtr client)
{
  xVncExtGetClientCutTextReply rep;

  REQUEST_SIZE_MATCH(xVncExtGetClientCutTextReq);

  rep.type = X_Reply;
  rep.length = (clientCutTextLen + 3) >> 2;
  rep.sequenceNumber = client->sequence;
  rep.textLen = clientCutTextLen;
  if (client->swapped) {
#if XORG < 112
    int n;
    swaps(&rep.sequenceNumber, n);
    swapl(&rep.length, n);
    swapl(&rep.textLen, n);
#else
    swaps(&rep.sequenceNumber);
    swapl(&rep.length);
    swapl(&rep.textLen);
#endif
  }
  WriteToClient(client, sizeof(xVncExtGetClientCutTextReply), (char *)&rep);
  if (clientCutText)
    WriteToClient(client, clientCutTextLen, clientCutText);
  return (client->noClientException);
}

static int SProcVncExtGetClientCutText(ClientPtr client)
{
  REQUEST(xVncExtGetClientCutTextReq);
#if XORG < 112
  register char n;
  swaps(&stuff->length, n);
#else
  swaps(&stuff->length);
#endif
  REQUEST_SIZE_MATCH(xVncExtGetClientCutTextReq);
  return ProcVncExtGetClientCutText(client);
}

static int ProcVncExtSelectInput(ClientPtr client)
{
  struct VncInputSelect** nextPtr;
  struct VncInputSelect* cur;
  REQUEST(xVncExtSelectInputReq);
  REQUEST_SIZE_MATCH(xVncExtSelectInputReq);
  nextPtr = &vncInputSelectHead;
  for (cur = vncInputSelectHead; cur; cur = *nextPtr) {
    if (cur->client == client && cur->window == stuff->window) {
      cur->mask = stuff->mask;
      if (!cur->mask) {
        *nextPtr = cur->next;
        free(cur);
      }
      break;
    }
    nextPtr = &cur->next;
  }
  if (!cur) {
    cur = malloc(sizeof(struct VncInputSelect));
    if (cur == NULL)
      return BadAlloc;
    memset(cur, 0, sizeof(struct VncInputSelect));

    cur->client = client;
    cur->window = stuff->window;
    cur->mask = stuff->mask;

    cur->next = vncInputSelectHead;
    vncInputSelectHead = cur;
  }
  return (client->noClientException);
}

static int SProcVncExtSelectInput(ClientPtr client)
{
  REQUEST(xVncExtSelectInputReq);
#if XORG < 112
  register char n;
  swaps(&stuff->length, n);
#else
  swaps(&stuff->length);
#endif
  REQUEST_SIZE_MATCH(xVncExtSelectInputReq);
#if XORG < 112
  swapl(&stuff->window, n);
  swapl(&stuff->mask, n);
#else
  swapl(&stuff->window);
  swapl(&stuff->mask);
#endif
  return ProcVncExtSelectInput(client);
}

static int ProcVncExtConnect(ClientPtr client)
{
  char *address;
  xVncExtConnectReply rep;

  REQUEST(xVncExtConnectReq);
  REQUEST_FIXED_SIZE(xVncExtConnectReq, stuff->strLen);

  address = malloc(stuff->strLen+1);
  if (address == NULL)
    return BadAlloc;
  strncpy(address, (char*)&stuff[1], stuff->strLen);
  address[stuff->strLen] = 0;

  rep.success = 0;
  if (vncConnectClient(address) == 0)
        rep.success = 1;

  rep.type = X_Reply;
  rep.length = 0;
  rep.sequenceNumber = client->sequence;
  if (client->swapped) {
#if XORG < 112
    int n;
    swaps(&rep.sequenceNumber, n);
    swapl(&rep.length, n);
#else
    swaps(&rep.sequenceNumber);
    swapl(&rep.length);
#endif
  }
  WriteToClient(client, sizeof(xVncExtConnectReply), (char *)&rep);

  free(address);

  return (client->noClientException);
}

static int SProcVncExtConnect(ClientPtr client)
{
  REQUEST(xVncExtConnectReq);
#if XORG < 112
  register char n;
  swaps(&stuff->length, n);
#else
  swaps(&stuff->length);
#endif
  REQUEST_AT_LEAST_SIZE(xVncExtConnectReq);
  return ProcVncExtConnect(client);
}


static int ProcVncExtGetQueryConnect(ClientPtr client)
{
  uint32_t opaqueId;
  const char *qcAddress, *qcUsername;
  int qcTimeout;

  xVncExtGetQueryConnectReply rep;

  REQUEST_SIZE_MATCH(xVncExtGetQueryConnectReq);

  vncGetQueryConnect(&opaqueId, &qcAddress, &qcUsername, &qcTimeout);

  rep.type = X_Reply;
  rep.sequenceNumber = client->sequence;
  rep.timeout = qcTimeout;
  rep.addrLen = qcTimeout ? strlen(qcAddress) : 0;
  rep.userLen = qcTimeout ? strlen(qcUsername) : 0;
  rep.opaqueId = (CARD32)(long)opaqueId;
  rep.length = ((rep.userLen + 3) >> 2) + ((rep.addrLen + 3) >> 2);
  if (client->swapped) {
#if XORG < 112
    int n;
    swaps(&rep.sequenceNumber, n);
    swapl(&rep.addrLen, n);
    swapl(&rep.userLen, n);
    swapl(&rep.timeout, n);
    swapl(&rep.opaqueId, n);
    swapl(&rep.length, n);
#else
    swaps(&rep.sequenceNumber);
    swapl(&rep.addrLen);
    swapl(&rep.userLen);
    swapl(&rep.timeout);
    swapl(&rep.opaqueId);
    swapl(&rep.length);
#endif
  }
  WriteToClient(client, sizeof(xVncExtGetQueryConnectReply), (char *)&rep);
  if (qcTimeout)
    WriteToClient(client, strlen(qcAddress), qcAddress);
  if (qcTimeout)
    WriteToClient(client, strlen(qcUsername), qcUsername);
  return (client->noClientException);
}

static int SProcVncExtGetQueryConnect(ClientPtr client)
{
  REQUEST(xVncExtGetQueryConnectReq);
#if XORG < 112
  register char n;
  swaps(&stuff->length, n);
#else
  swaps(&stuff->length);
#endif
  REQUEST_SIZE_MATCH(xVncExtGetQueryConnectReq);
  return ProcVncExtGetQueryConnect(client);
}


static int ProcVncExtApproveConnect(ClientPtr client)
{
  REQUEST(xVncExtApproveConnectReq);
  REQUEST_SIZE_MATCH(xVncExtApproveConnectReq);
  vncApproveConnection(stuff->opaqueId, stuff->approve);
  // Inform other clients of the event and tidy up
  vncNotifyQueryConnect();
  return (client->noClientException);
}

static int SProcVncExtApproveConnect(ClientPtr client)
{
  REQUEST(xVncExtApproveConnectReq);
#if XORG < 112
  register char n;
  swaps(&stuff->length, n);
  swapl(&stuff->opaqueId, n);
#else
  swaps(&stuff->length);
  swapl(&stuff->opaqueId);
#endif
  REQUEST_SIZE_MATCH(xVncExtApproveConnectReq);
  return ProcVncExtApproveConnect(client);
}


static int ProcVncExtDispatch(ClientPtr client)
{
  REQUEST(xReq);
  switch (stuff->data) {
  case X_VncExtSetParam:
    return ProcVncExtSetParam(client);
  case X_VncExtGetParam:
    return ProcVncExtGetParam(client);
  case X_VncExtGetParamDesc:
    return ProcVncExtGetParamDesc(client);
  case X_VncExtListParams:
    return ProcVncExtListParams(client);
  case X_VncExtSetServerCutText:
    return ProcVncExtSetServerCutText(client);
  case X_VncExtGetClientCutText:
    return ProcVncExtGetClientCutText(client);
  case X_VncExtSelectInput:
    return ProcVncExtSelectInput(client);
  case X_VncExtConnect:
    return ProcVncExtConnect(client);
  case X_VncExtGetQueryConnect:
    return ProcVncExtGetQueryConnect(client);
  case X_VncExtApproveConnect:
    return ProcVncExtApproveConnect(client);
  default:
    return BadRequest;
  }
}

static int SProcVncExtDispatch(ClientPtr client)
{
  REQUEST(xReq);
  switch (stuff->data) {
  case X_VncExtSetParam:
    return SProcVncExtSetParam(client);
  case X_VncExtGetParam:
    return SProcVncExtGetParam(client);
  case X_VncExtGetParamDesc:
    return SProcVncExtGetParamDesc(client);
  case X_VncExtListParams:
    return SProcVncExtListParams(client);
  case X_VncExtSetServerCutText:
    return SProcVncExtSetServerCutText(client);
  case X_VncExtGetClientCutText:
    return SProcVncExtGetClientCutText(client);
  case X_VncExtSelectInput:
    return SProcVncExtSelectInput(client);
  case X_VncExtConnect:
    return SProcVncExtConnect(client);
  case X_VncExtGetQueryConnect:
    return SProcVncExtGetQueryConnect(client);
  case X_VncExtApproveConnect:
    return SProcVncExtApproveConnect(client);
  default:
    return BadRequest;
  }
}

static void vncResetProc(ExtensionEntry* extEntry)
{
}

static void vncClientStateChange(CallbackListPtr * l, void * d, void * p)
{
  ClientPtr client = ((NewClientInfoRec*)p)->client;
  if (client->clientState == ClientStateGone) {
    struct VncInputSelect** nextPtr = &vncInputSelectHead;
    for (struct VncInputSelect* cur = vncInputSelectHead; cur; cur = *nextPtr) {
      if (cur->client == client) {
        *nextPtr = cur->next;
        free(cur);
        continue;
      }
      nextPtr = &cur->next;
    }
  }
}

static void SendSelectionChangeEvent(Atom selection)
{
  xVncExtSelectionChangeNotifyEvent ev;
  ev.type = vncEventBase + VncExtSelectionChangeNotify;
  for (struct VncInputSelect* cur = vncInputSelectHead; cur; cur = cur->next) {
    if (cur->mask & VncExtSelectionChangeMask) {
      ev.sequenceNumber = cur->client->sequence;
      ev.window = cur->window;
      ev.selection = selection;
      if (cur->client->swapped) {
#if XORG < 112
        int n;
        swaps(&ev.sequenceNumber, n);
        swapl(&ev.window, n);
        swapl(&ev.selection, n);
#else
        swaps(&ev.sequenceNumber);
        swapl(&ev.window);
        swapl(&ev.selection);
#endif
      }
      WriteToClient(cur->client, sizeof(xVncExtSelectionChangeNotifyEvent),
                    (char *)&ev);
    }
  }
}

static void vncSelectionCallback(CallbackListPtr *callbacks, void * data, void * args)
{
  SelectionInfoRec *info = (SelectionInfoRec *) args;
  Selection *selection = info->selection;

  SendSelectionChangeEvent(selection->selection);
}

