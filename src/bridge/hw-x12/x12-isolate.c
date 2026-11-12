// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// the x11 window isolation: a client sees only itself and the windows
// of its own process tree. x11 window ids are global and guessable by
// design, so the bridge cloaks every lookup of a foreign window as
// BadWindow - the same answer a nonexistent id gets. the model follows
// the x12 canon: a client sees nothing it was not granted.
//
// the grants:
//   the manager - the holder of SubstructureRedirect on the root -
//   sees the whole tree, fvwm and taskbars need it
//   process names listed in X12_BRIDGE_GRANTS get the same full view
//   everyone else sees own windows plus windows of child processes,
//   tracked through the peer credential of the unix socket and the
//   /proc parent chain
//
// request policy beyond the cloak:
//   QueryTree on root lists only your own toplevels and the parent
//   field of a framed window lies and says root - a window believes
//   it sits alone on the screen
//   QueryPointer reports motion only inside your own windows and
//   refuses the root query, xeyes stops following the cursor the
//   moment it leaves the eye window
//   TranslateCoords maps through the same "alone on the screen" lie
//   GetImage on root is the screencopy privilege, manager only
//   QueryKeymap lies an empty map
//   SendEvent client messages may follow the manager channel (the
//   ewmh request path), synthetic input is manager only
//   root grabs and GrabServer are manager only
//
// known limits of this wave: swapped (big endian) clients bypass the
// request wraps - the lookup cloak still holds for them - and window
// event payloads are not scrubbed yet (above-sibling ids can leak)

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <X11/X.h>
#include <X11/Xproto.h>

#include "os.h"
#include "dix.h"
#include "dixstruct.h"
#include "scrnintstr.h"
#include "windowstr.h"
#include "inputstr.h"
#include "input.h"
#include "inpututils.h"
#include "resource.h"
#include "dixaccess.h"
#include "swaprep.h"
#include "xace.h"
#include "xacestr.h"
#include "os/osdep.h"

#define SAME_SCREENS(a, b) \
    ((a).pScreen == (b).pScreen && (a).depth == (b).depth)

// the dix request procs have no public header, they live in the
// dispatch tables
extern int ProcQueryKeymap(ClientPtr client);
extern int ProcSendEvent(ClientPtr client);

#include "x12-isolate.h"

// per client isolation state, keyed by the client index. pids are
// resolved lazily on the first lookup so the table needs no connect
// hook, dead entries die with the client slot
struct iso_client {
    pid_t pid;              // peer pid of the unix socket, 0 = unknown
    int resolved;
    int grant_env;          // the process name is in X12_BRIDGE_GRANTS
    int grant_mgr;          // holds SubstructureRedirect on the root
};

static struct iso_client iso_clients[MAXCLIENTS];

// pid to ppid pairs so the process tree walk stays cheap
struct iso_pp {
    pid_t pid;
    pid_t ppid;
};
static struct iso_pp iso_pp[64];

// the wrapped request procs
static int (*orig_query_tree)(ClientPtr);
static int (*orig_translate_coords)(ClientPtr);
static int (*orig_send_event)(ClientPtr);
static int (*orig_change_window_attributes)(ClientPtr);
static int (*orig_grab_server)(ClientPtr);

static pid_t
iso_peer_pid(ClientPtr client)
{
    OsCommPtr oc = (OsCommPtr)client->osPrivate;
    struct ucred cred;
    socklen_t len = sizeof(cred);

    if (!oc || oc->fd < 0) {
        return 0;
    }
    if (getsockopt(oc->fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
        return 0;
    }
    return cred.pid;
}

// the process name lives in /proc/pid/comm, short and newline capped
static int
iso_granted(pid_t pid)
{
    const char *env = getenv("X12_BRIDGE_GRANTS");
    char path[64], comm[64];
    FILE *f;

    if (!env || !*env || pid <= 0) {
        return 0;
    }
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
    f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    if (!fgets(comm, sizeof(comm), f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    comm[strcspn(comm, "\n")] = 0;
    for (const char *p = env; *p; ) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == strlen(comm) && !strncmp(p, comm, n)) {
            return 1;
        }
        if (!e) {
            break;
        }
        p = e + 1;
    }
    return 0;
}

static pid_t
iso_ppid(pid_t pid)
{
    char path[64], buf[1024];
    char *rp;
    int ppid = 0;
    FILE *f;
    size_t got;

    if (pid <= 1) {
        return 0;
    }
    for (int i = 0; i < 64; i++) {
        if (iso_pp[i].pid == pid && iso_pp[i].ppid > 0) {
            return iso_pp[i].ppid;
        }
    }
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = 0;
    // the comm field is parenthesized and may hold spaces, the state
    // and parent pid sit right after the closing paren
    rp = strrchr(buf, ')');
    if (!rp || sscanf(rp + 1, " %*c %d", &ppid) != 1) {
        return 0;
    }
    struct iso_pp *slot = &iso_pp[((unsigned)pid ^ ((unsigned)pid >> 6)) & 63];
    slot->pid = pid;
    slot->ppid = (pid_t)ppid;
    return (pid_t)ppid;
}

// is "pid" the ancestor itself or lives below it in the process tree
static int
iso_is_descendant(pid_t pid, pid_t ancestor)
{
    if (pid <= 0 || ancestor <= 0) {
        return 0;
    }
    for (int depth = 0; depth < 64; depth++) {
        if (pid == ancestor) {
            return 1;
        }
        pid = iso_ppid(pid);
        if (pid <= 1) {
            return 0;
        }
    }
    return 0;
}

static struct iso_client *
iso_client(ClientPtr client)
{
    int idx = client->index;
    struct iso_client *ic;

    if (idx < 0 || idx >= MAXCLIENTS) {
        return NULL;
    }
    ic = &iso_clients[idx];
    if (!ic->resolved) {
        ic->resolved = 1;
        ic->pid = iso_peer_pid(client);
        ic->grant_env = iso_granted(ic->pid);
    }
    return ic;
}

static int
iso_view_all(ClientPtr client)
{
    struct iso_client *ic = iso_client(client);
    return ic && (ic->grant_env || ic->grant_mgr);
}

// the pid behind a client index, 0 when the client is gone or came in
// over a transport without peer credentials
static pid_t
iso_pid_of_index(int idx)
{
    if (idx <= 0 || idx >= MAXCLIENTS || !clients[idx]) {
        return 0;
    }
    struct iso_client *ic = iso_client(clients[idx]);
    return ic ? ic->pid : 0;
}

// the core rule: own windows, server minted windows, windows of your
// own process tree. everything else is a wall
static int
iso_window_visible(ClientPtr client, WindowPtr w)
{
    if (w->parent == NULL) {
        return 1;               // the root, special cased per request
    }
    if (iso_view_all(client)) {
        return 1;
    }
    int owner = CLIENT_ID(w->drawable.id);
    if (owner == 0) {
        return 1;               // server minted windows
    }
    if (owner == client->index) {
        return 1;
    }
    pid_t owner_pid = iso_pid_of_index(owner);
    if (owner_pid == 0 || client->index < 0) {
        return 0;               // unknown owner, fail closed
    }
    struct iso_client *ic = iso_client(client);
    if (!ic || ic->pid <= 0) {
        return 0;
    }
    if (owner_pid == ic->pid) {
        return 1;
    }
    // windows of child processes belong to the family view
    return iso_is_descendant(owner_pid, ic->pid);
}

// the highest window of your own world - the toplevel you live in,
// stopped at the first ancestor the cloak hides (the wm frame). the
// "alone on the screen" lie places this window at the screen origin
static WindowPtr
iso_own_toplevel(ClientPtr client, WindowPtr w)
{
    while (w->parent && w->parent->parent &&
           iso_window_visible(client, w->parent)) {
        w = w->parent;
    }
    return w;
}

// a client message may follow the manager channel: to the root (the
// ewmh request path) or to a window the manager owns. synthetic
// input never gets this shortcut
static int
iso_send_channel(ClientPtr client, WindowPtr w)
{
    xSendEventReq *req = (xSendEventReq *)client->requestBuffer;
    CARD8 etype = *(CARD8 *)&req->event;
    int owner;

    if (etype != ClientMessage) {
        return 0;
    }
    if (w->parent == NULL) {
        return 1;
    }
    if (iso_window_visible(client, w)) {
        return 1;
    }
    owner = CLIENT_ID(w->drawable.id);
    if (owner <= 0 || owner >= MAXCLIENTS || !clients[owner]) {
        return 0;
    }
    return iso_view_all(clients[owner]);
}

// the lookup cloak: every window lookup of dix funnels through this
// hook with the access mode the request asked for
static void
iso_resource(CallbackListPtr *pcbl, void *priv, void *data)
{
    XaceResourceAccessRec *rec = (XaceResourceAccessRec *)data;
    ClientPtr client = rec->client;
    WindowPtr w;

    (void)pcbl;
    (void)priv;
    if (!client || client->index == 0 || !rec->res) {
        return;
    }
    if (rec->rtype != X11_RESTYPE_WINDOW) {
        return;
    }
    w = (WindowPtr)rec->res;

    if (client->majorOp == X_SendEvent && iso_send_channel(client, w)) {
        return;
    }

    if (w->parent == NULL) {
        // the root stays visible, but screen reads and root grabs are
        // privileges the manager holds
        if (!iso_view_all(client)) {
            if (rec->access_mode & DixReadAccess) {
                rec->status = BadAccess;
            }
            if ((rec->access_mode & DixSetAttrAccess) &&
                (client->majorOp == X_GrabPointer ||
                 client->majorOp == X_GrabKeyboard ||
                 client->majorOp == X_GrabButton ||
                 client->majorOp == X_GrabKey)) {
                rec->status = BadAccess;
            }
        }
        return;
    }

    if (!iso_window_visible(client, w)) {
        // BadValue maps to the type error value, BadWindow for windows
        rec->status = BadValue;
    }
}

// QueryTree lists only your own children and claims the root is the
// parent of your toplevels - the window believes it is unframed
static int
iso_query_tree(ClientPtr client)
{
    xQueryTreeReply rep;
    WindowPtr pWin, pChild, pHead, pParent;
    Window *ids = NULL;
    Window parent_id;
    int rc, n = 0;

    REQUEST(xResourceReq);
    REQUEST_SIZE_MATCH(xResourceReq);
    rc = dixLookupWindow(&pWin, stuff->id, client, DixListAccess);
    if (rc != Success) {
        return rc;
    }

    pHead = RealChildHead(pWin);
    for (pChild = pWin->lastChild; pChild != pHead; pChild = pChild->prevSib) {
        if (iso_window_visible(client, pChild)) {
            n++;
        }
    }
    if (n) {
        int i = 0;
        ids = calloc((size_t)n, sizeof(Window));
        if (!ids) {
            return BadAlloc;
        }
        for (pChild = pWin->lastChild; pChild != pHead;
             pChild = pChild->prevSib) {
            if (iso_window_visible(client, pChild)) {
                ids[i++] = pChild->drawable.id;
            }
        }
    }

    pParent = pWin->parent;
    if (!pParent) {
        parent_id = None;
    }
    else if (iso_view_all(client) || iso_window_visible(client, pParent)) {
        parent_id = pParent->drawable.id;
    }
    else {
        // the alone lie: behind your toplevel there is only the root
        parent_id = pParent->drawable.pScreen->root->drawable.id;
    }

    rep = (xQueryTreeReply) {
        .type = X_Reply,
        .sequenceNumber = client->sequence,
        .length = bytes_to_int32((long)n * sizeof(Window)),
        .root = pWin->drawable.pScreen->root->drawable.id,
        .parent = parent_id,
        .nChildren = n
    };
    WriteReplyToClient(client, sizeof(rep), &rep);
    if (n) {
        client->pSwapReplyFunc = (ReplySwapPtr)Swap32Write;
        WriteSwappedDataToClient(client, (long)n * sizeof(Window), ids);
        free(ids);
    }
    return Success;
}

// QueryPointer: the cursor exists only inside your own windows. the
// root query and the outside case answer "another screen" (the xeyes
// freeze), the inside case reports your own coordinates and hides the
// screen position behind the alone lie
static int
iso_query_pointer(ClientPtr client)
{
    xQueryPointerReply rep;
    WindowPtr pWin, t;
    DeviceIntPtr mouse = PickPointer(client);
    DeviceIntPtr keyboard;
    SpritePtr pSprite;
    struct iso_client *ic;
    int show = 0, rc;

    REQUEST(xResourceReq);
    REQUEST_SIZE_MATCH(xResourceReq);
    rc = dixLookupWindow(&pWin, stuff->id, client, DixGetAttrAccess);
    if (rc != Success) {
        return rc;
    }

    keyboard = GetMaster(mouse, MASTER_KEYBOARD);
    pSprite = mouse->spriteInfo->sprite;
    if (mouse->valuator->motionHintWindow) {
        MaybeStopHint(mouse, client);
    }
    ic = iso_client(client);

    rep = (xQueryPointerReply) {
        .type = X_Reply,
        .sequenceNumber = client->sequence,
        .length = 0,
        .mask = 0,
        .root = (GetCurrentRootWindow(mouse))->drawable.id,
        .rootX = pSprite->hot.x,
        .rootY = pSprite->hot.y,
        .child = None,
        .sameScreen = xFalse
    };

    if (ic && (ic->grant_env || ic->grant_mgr)) {
        show = 1;               // the manager answer is the true one
    }
    else if (pWin->parent != NULL &&
             pSprite->hot.pScreen == pWin->drawable.pScreen) {
        int wx = pSprite->hot.x - pWin->drawable.x;
        int wy = pSprite->hot.y - pWin->drawable.y;
        if (wx >= 0 && wx < (int)pWin->drawable.width &&
            wy >= 0 && wy < (int)pWin->drawable.height) {
            show = 1;
            rep.winX = wx;
            rep.winY = wy;
            // the alone lie: your window sits at the screen origin
            rep.rootX = wx;
            rep.rootY = wy;
            for (t = pSprite->win; t; t = t->parent) {
                if (t->parent == pWin) {
                    rep.child = t->drawable.id;
                    break;
                }
            }
        }
    }

    if (show) {
        rep.sameScreen = xTrue;
        rep.mask = event_get_corestate(mouse, keyboard);
        if (ic && (ic->grant_env || ic->grant_mgr)) {
            rep.winX = pSprite->hot.x - pWin->drawable.x;
            rep.winY = pSprite->hot.y - pWin->drawable.y;
            rep.rootX = pSprite->hot.x;
            rep.rootY = pSprite->hot.y;
            for (t = pSprite->win; t; t = t->parent) {
                if (t->parent == pWin) {
                    rep.child = t->drawable.id;
                    break;
                }
            }
        }
    }

    WriteReplyToClient(client, sizeof(rep), &rep);
    return Success;
}

// TranslateCoords runs through the alone lie: to the root your world
// is placed at the origin, from the root the same shift applies in
// reverse. own to own translations are plain geometry
static int
iso_translate_coords(ClientPtr client)
{
    xTranslateCoordsReply rep;
    WindowPtr pWin, pDst;
    struct iso_client *ic;
    int rc;

    REQUEST(xTranslateCoordsReq);
    REQUEST_SIZE_MATCH(xTranslateCoordsReq);
    rc = dixLookupWindow(&pWin, stuff->srcWid, client, DixGetAttrAccess);
    if (rc != Success) {
        return rc;
    }
    rc = dixLookupWindow(&pDst, stuff->dstWid, client, DixGetAttrAccess);
    if (rc != Success) {
        return rc;
    }

    rep = (xTranslateCoordsReply) {
        .type = X_Reply,
        .sequenceNumber = client->sequence,
        .length = 0
    };
    if (!SAME_SCREENS(pWin->drawable, pDst->drawable)) {
        rep.sameScreen = xFalse;
        rep.child = None;
        rep.dstX = rep.dstY = 0;
    }
    else {
        INT16 x, y;

        rep.sameScreen = xTrue;
        rep.child = None;
        /* computing absolute coordinates -- adjust to destination later */
        x = pWin->drawable.x + stuff->srcX;
        y = pWin->drawable.y + stuff->srcY;

        ic = iso_client(client);
        if (ic && !(ic->grant_env || ic->grant_mgr)) {
            WindowPtr root = pDst->drawable.pScreen->root;
            if (pDst == root) {
                WindowPtr tl = iso_own_toplevel(client, pWin);
                x = (INT16)(x - tl->drawable.x);
                y = (INT16)(y - tl->drawable.y);
            }
            else if (pWin == root) {
                WindowPtr tl = iso_own_toplevel(client, pDst);
                x = (INT16)(x + tl->drawable.x);
                y = (INT16)(y + tl->drawable.y);
            }
        }

        pWin = pDst->firstChild;
        while (pWin) {
            BoxRec box;

            if ((pWin->mapped) &&
                (x >= pWin->drawable.x - wBorderWidth(pWin)) &&
                (x < pWin->drawable.x + (int)pWin->drawable.width +
                 wBorderWidth(pWin)) &&
                (y >= pWin->drawable.y - wBorderWidth(pWin)) &&
                (y < pWin->drawable.y + (int)pWin->drawable.height +
                 wBorderWidth(pWin))
                && (!wBoundingShape(pWin) ||
                    RegionContainsPoint(&pWin->borderSize, x, y, &box))
                && (!wInputShape(pWin) ||
                    RegionContainsPoint(wInputShape(pWin),
                                        x - pWin->drawable.x,
                                        y - pWin->drawable.y, &box))
                ) {
                if (iso_window_visible(client, pWin)) {
                    rep.child = pWin->drawable.id;
                }
                pWin = (WindowPtr)NULL;
            }
            else {
                pWin = pWin->nextSib;
            }
        }
        rep.dstX = (INT16)(x - pDst->drawable.x);
        rep.dstY = (INT16)(y - pDst->drawable.y);
    }

    WriteReplyToClient(client, sizeof(rep), &rep);
    return Success;
}

// GetInputFocus hides foreign focus behind None: you learn whether
// you are focused, never where the focus went
static int
iso_get_input_focus(ClientPtr client)
{
    DeviceIntPtr kbd = PickKeyboard(client);
    xGetInputFocusReply rep;
    FocusClassPtr focus = kbd->focus;

    REQUEST_SIZE_MATCH(xReq);
    rep = (xGetInputFocusReply) {
        .type = X_Reply,
        .length = 0,
        .sequenceNumber = client->sequence,
        .revertTo = focus->revert
    };
    if (focus->win == NoneWin) {
        rep.focus = None;
    }
    else if (focus->win == PointerRootWin) {
        rep.focus = PointerRoot;
    }
    else if (iso_window_visible(client, focus->win)) {
        rep.focus = focus->win->drawable.id;
    }
    else {
        rep.focus = None;
    }
    WriteReplyToClient(client, sizeof(rep), &rep);
    return Success;
}

// QueryKeymap is the keylogger request - the manager reads the key
// state, everyone else gets the empty map
static int
iso_query_keymap(ClientPtr client)
{
    xQueryKeymapReply rep;
    struct iso_client *ic = iso_client(client);

    REQUEST_SIZE_MATCH(xReq);
    if (ic && (ic->grant_env || ic->grant_mgr)) {
        return ProcQueryKeymap(client);
    }
    memset(&rep, 0, sizeof(rep));
    rep.type = X_Reply;
    rep.sequenceNumber = client->sequence;
    rep.length = 2;
    WriteReplyToClient(client, sizeof(rep), &rep);
    return Success;
}

// SendEvent: a client message may ride the manager channel (ewmh
// requests), synthetic input events are manager only. the lookup
// cloak already blinds the request to peer windows
static int
iso_send_event(ClientPtr client)
{
    Window w = None;
    WindowPtr pWin;
    CARD8 etype;
    int rc;

    REQUEST(xSendEventReq);
    REQUEST_SIZE_MATCH(xSendEventReq);
    etype = *(CARD8 *)&stuff->event;

    // a client message is the ewmh request path and travels for
    // everyone: the destination checks below police synthetic input
    // only. the order matters - a message aimed at the pointer or the
    // focus window is the normal ewmh way and must not look like a
    // synthetic input attempt
    if (etype == ClientMessage) {
        return ProcSendEvent(client);
    }

    if (stuff->destination == PointerWindow ||
        stuff->destination == InputFocus) {
        // targeting whatever holds the pointer or the focus is a
        // manager channel
        return iso_view_all(client) ? ProcSendEvent(client) : BadAccess;
    }

    w = stuff->destination;
    if (w == None) {
        return BadValue;
    }
    rc = dixLookupWindow(&pWin, w, client, DixGetAttrAccess);
    if (rc != Success) {
        return rc;
    }
    if (pWin->parent == NULL && !iso_view_all(client)) {
        // synthetic input at the root would land on whoever holds the
        // pointer next
        return BadAccess;
    }
    return ProcSendEvent(client);
}

// ChangeWindowAttributes watches the root event mask: the redirect
// holder is the manager, and root input/substructure watches are the
// manager's privilege
static int
iso_change_window_attributes(ClientPtr client)
{
    xChangeWindowAttributesReq *req;
    CARD32 *values;
    CARD32 evmask = 0;
    WindowPtr pWin;
    Mask forbidden;
    int rc, claim = 0;
    unsigned int bit;

    REQUEST(xChangeWindowAttributesReq);
    REQUEST_AT_LEAST_SIZE(xChangeWindowAttributesReq);

    req = (xChangeWindowAttributesReq *)client->requestBuffer;
    rc = dixLookupWindow(&pWin, req->window, client, DixGetAttrAccess);
    if (rc != Success || pWin->parent != NULL) {
        // foreign and own windows: the plain path, the cloak handles
        // the foreign case
        return orig_change_window_attributes(client);
    }

    // the value list holds one card32 per set bit of the mask in bit
    // order, the event mask lives at its own bit (0x800). the mask
    // constants are bit values and not list indexes - walking them as
    // indexes missed the slot every time and no client ever became
    // the manager
    values = (CARD32 *)(req + 1);
    unsigned int maxv =
        (client->req_len * 4 - sizeof(xChangeWindowAttributesReq)) / 4;
    unsigned int seen = 0;
    for (bit = 0; bit < 15; bit++) {
        if (req->valueMask & (1u << bit)) {
            if (seen >= maxv) {
                break;
            }
            if ((1u << bit) == CWEventMask) {
                evmask = *values;
            }
            values++;
            seen++;
        }
    }

    if (req->valueMask & CWEventMask) {
        if (evmask & SubstructureRedirectMask) {
            claim = 1;
        }
    }

    if (!claim && !iso_view_all(client)) {
        forbidden = SubstructureNotifyMask | SubstructureRedirectMask |
            ResizeRedirectMask | KeyPressMask | KeyReleaseMask |
            ButtonPressMask | ButtonReleaseMask | EnterWindowMask |
            LeaveWindowMask | PointerMotionMask | PointerMotionHintMask |
            Button1MotionMask | Button2MotionMask | Button3MotionMask |
            Button4MotionMask | Button5MotionMask | ButtonMotionMask |
            KeymapStateMask;
        if ((req->valueMask & CWEventMask) && (evmask & forbidden)) {
            return BadAccess;
        }
    }

    rc = orig_change_window_attributes(client);
    if (rc != Success) {
        return rc;
    }

    struct iso_client *ic = iso_client(client);
    if (ic && claim) {
        // the redirect holder is the manager, dix already guaranteed
        // the hold is exclusive. the grant sticks to the connection:
        // toolkits reshuffle the root mask behind the wm's back (gdk
        // re selects a plain key and property mask after the claim)
        // and a demotion on that would hand the wm a cloaked view of
        // its own desktop. the claimer already saw the whole tree the
        // moment it took the seat, sticking reveals nothing new
        ic->grant_mgr = 1;
    }
    return Success;
}

// freezing the server is a manager move
static int
iso_grab_server(ClientPtr client)
{
    if (!iso_view_all(client)) {
        return BadAccess;
    }
    return orig_grab_server(client);
}

void
x12_isolate_init(void)
{
    memset(iso_clients, 0, sizeof(iso_clients));
    memset(iso_pp, 0, sizeof(iso_pp));

    XaceRegisterCallback(XACE_RESOURCE_ACCESS, iso_resource, NULL);

    orig_query_tree = ProcVector[X_QueryTree];
    ProcVector[X_QueryTree] = iso_query_tree;

    ProcVector[X_QueryPointer] = iso_query_pointer;
    ProcVector[X_TranslateCoords] = iso_translate_coords;
    ProcVector[X_GetInputFocus] = iso_get_input_focus;
    ProcVector[X_QueryKeymap] = iso_query_keymap;

    orig_send_event = ProcVector[X_SendEvent];
    ProcVector[X_SendEvent] = iso_send_event;

    orig_change_window_attributes = ProcVector[X_ChangeWindowAttributes];
    ProcVector[X_ChangeWindowAttributes] = iso_change_window_attributes;

    orig_grab_server = ProcVector[X_GrabServer];
    ProcVector[X_GrabServer] = iso_grab_server;
}
