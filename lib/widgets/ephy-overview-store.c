/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 *  Copyright © 2012 Igalia S.L.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"
#include "ephy-history-service.h"
#include "ephy-overview-store.h"
#include "ephy-snapshot-service.h"

#define EPHY_OVERVIEW_STORE_GET_PRIVATE(object) (G_TYPE_INSTANCE_GET_PRIVATE ((object), EPHY_TYPE_OVERVIEW_STORE, EphyOverviewStorePrivate))

struct _EphyOverviewStorePrivate
{
  EphyHistoryService *history_service;
};

enum
{
  PROP_0,
  PROP_HISTORY_SERVICE,
};

G_DEFINE_TYPE (EphyOverviewStore, ephy_overview_store, GTK_TYPE_LIST_STORE)

static void
ephy_overview_store_set_property (GObject *object,
                                  guint prop_id,
                                  const GValue *value,
                                  GParamSpec *pspec)
{
  EphyOverviewStore *store = EPHY_OVERVIEW_STORE (object);

  switch (prop_id)
  {
  case PROP_HISTORY_SERVICE:
    store->priv->history_service = g_value_get_object (value);
    g_object_notify (object, "history-service");
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    break;
  }
}

static void
ephy_overview_store_get_property (GObject *object,
                                  guint prop_id,
                                  GValue *value,
                                  GParamSpec *pspec)
{
  EphyOverviewStore *store = EPHY_OVERVIEW_STORE (object);

  switch (prop_id)
  {
  case PROP_HISTORY_SERVICE:
    g_value_set_object (value, store->priv->history_service);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    break;
  }
}

static void
ephy_overview_store_class_init (EphyOverviewStoreClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = ephy_overview_store_set_property;
  object_class->get_property = ephy_overview_store_get_property;

  g_object_class_install_property (object_class,
                                   PROP_HISTORY_SERVICE,
                                   g_param_spec_object ("history-service",
                                                        "History service",
                                                        "History Service",
                                                        EPHY_TYPE_HISTORY_SERVICE,
                                                        G_PARAM_READWRITE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));

  g_type_class_add_private (object_class, sizeof(EphyOverviewStorePrivate));
}

static void
ephy_overview_store_init (EphyOverviewStore *self)
{
  GType types[EPHY_OVERVIEW_STORE_NCOLS];

  types[EPHY_OVERVIEW_STORE_ID] = G_TYPE_STRING;
  types[EPHY_OVERVIEW_STORE_URI] = G_TYPE_STRING;
  types[EPHY_OVERVIEW_STORE_TITLE] = G_TYPE_STRING;
  types[EPHY_OVERVIEW_STORE_AUTHOR] = G_TYPE_STRING;
  types[EPHY_OVERVIEW_STORE_SNAPSHOT] = GDK_TYPE_PIXBUF;
  types[EPHY_OVERVIEW_STORE_LAST_VISIT] = G_TYPE_LONG;
  types[EPHY_OVERVIEW_STORE_SELECTED] = G_TYPE_BOOLEAN;

  gtk_list_store_set_column_types (GTK_LIST_STORE (self),
                                   EPHY_OVERVIEW_STORE_NCOLS, types);

  self->priv = EPHY_OVERVIEW_STORE_GET_PRIVATE (self);
}

typedef struct {
  GtkTreeRowReference *ref;
  char *url;
  WebKitWebView *webview;
} PeekContext;

static void
peek_context_free (PeekContext *ctx)
{
  g_free (ctx->url);
  gtk_tree_row_reference_free (ctx->ref);
  if (ctx->webview)
    g_object_unref (ctx->webview);

  g_slice_free (PeekContext, ctx);
}

static void
on_snapshot_retrieved_cb (GObject *object,
                          GAsyncResult *res,
                          PeekContext *ctx)
{
  GtkTreeModel *model;
  GtkTreePath *path;
  GtkTreeIter iter;
  char *url;
  GdkPixbuf *snapshot;
  GError *error = NULL;

  snapshot = ephy_snapshot_service_get_snapshot_finish (EPHY_SNAPSHOT_SERVICE (object),
                                                        res, &error);

  if (error) {
    g_warning ("Error retrieving snapshot: %s\n", error->message);
    g_error_free (error);
    error = NULL;
  } else if (gtk_tree_row_reference_valid (ctx->ref)) {
    model = gtk_tree_row_reference_get_model (ctx->ref);
    path = gtk_tree_row_reference_get_path (ctx->ref);
    gtk_tree_model_get_iter (model, &iter, path);
    gtk_tree_path_free (path);

    /* Chances are that this callback is called after the URL in this
       row has already replaced with a new one.  Make sure that this
       is not the case. */
    gtk_tree_model_get (model, &iter, EPHY_OVERVIEW_STORE_URI, &url, -1);
    if (g_strcmp0 (url, ctx->url) == 0)
      gtk_list_store_set (GTK_LIST_STORE (model), &iter,
                          EPHY_OVERVIEW_STORE_SNAPSHOT, snapshot,
                          -1);

    g_free (url);
    g_object_unref (snapshot);
  }
  peek_context_free (ctx);
}

static void
history_service_url_cb (gpointer service,
                        gboolean success,
                        EphyHistoryURL *url,
                        PeekContext *ctx)
{
  EphySnapshotService *snapshot_service;
  int timestamp;

  snapshot_service = ephy_snapshot_service_get_default ();

  /* This is a bit of an abuse of the semantics of the mtime
   paramenter. Since the thumbnailing backend only takes the exact
   mtime of the thumbnailed file, we will use the visit count to
   generate a fake timestamp that will be increased every fifth
   visit. This way, we'll update the thumbnail every other fifth
   visit. */
  timestamp = success ? (url->visit_count / 5) : 0;

  ephy_snapshot_service_get_snapshot_async (snapshot_service,
                                            ctx->webview, ctx->url, timestamp, NULL,
                                            (GAsyncReadyCallback) on_snapshot_retrieved_cb,
                                            ctx);
  ephy_history_url_free (url);
}

void
ephy_overview_store_peek_snapshot (EphyOverviewStore *self,
                                   WebKitWebView *webview,
                                   GtkTreeIter *iter)
{
  char *url;
  GtkTreePath *path;
  PeekContext *ctx;

  gtk_tree_model_get (GTK_TREE_MODEL (self), iter,
                      EPHY_OVERVIEW_STORE_URI, &url,
                      -1);

  if (url == NULL)
    return;

  ctx = g_slice_new (PeekContext);
  path = gtk_tree_model_get_path (GTK_TREE_MODEL (self), iter);
  ctx->ref = gtk_tree_row_reference_new (GTK_TREE_MODEL (self), path);
  ctx->url = url;
  ctx->webview = webview ? g_object_ref (webview) : NULL;
  ephy_history_service_get_url (self->priv->history_service,
                                url, NULL, (EphyHistoryJobCallback)history_service_url_cb,
                                ctx);
  gtk_tree_path_free (path);
}