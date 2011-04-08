#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include "bencode.h"
#include "trg-file-parser.h"

#define my_print_errno(x) printf("%s: error (%d) %s\n", __func__, errno, x);

static trg_torrent_file_node
    *trg_torrent_file_node_insert(trg_torrent_file_node * top,
                                  be_node * file_node, guint index,
                                  gint64 * total_length)
{
    int i;
    trg_torrent_file_node *path_el_parent = top;
    be_node *file_length_node = be_dict_find(file_node, "length", BE_INT);
    be_node *file_path_node = be_dict_find(file_node, "path", BE_LIST);

    if (!file_path_node || !file_length_node)
        return NULL;

    /* Iterate over the path list which contains each file/directory
     * component of the path in order.
     */
    for (i = 0;;) {
        be_node *path_el_node = file_path_node->val.l[i];

        trg_torrent_file_node *target_node = NULL;
        GList *li;

        /* Does this element exist already? */
        for (li = path_el_parent->children; li != NULL;
             li = g_list_next(li)) {
            trg_torrent_file_node *x = (trg_torrent_file_node *) li->data;
            if (!g_strcmp0(x->name, path_el_node->val.s)) {
                target_node = x;
                break;
            }
        }

        if (!target_node) {
            /* Create a new node and add it as a child of the parent from the
             * last iteration. */
            target_node = g_new0(trg_torrent_file_node, 1);
            target_node->name = g_strdup(path_el_node->val.s);
            path_el_parent->children =
                g_list_append(path_el_parent->children, target_node);
        }

        path_el_parent = target_node;

        /* Is this the last component of the path (the file)? */
        if (!file_path_node->val.l[++i]) {
            *total_length += (target_node->length =
                              (gint64) (file_length_node->val.i));
            target_node->index = index;
            return target_node;
        }
    }
}

static void trg_torrent_file_node_free(trg_torrent_file_node * node)
{
    GList *li;
    for (li = node->children; li != NULL; li = g_list_next(li))
        trg_torrent_file_node_free((trg_torrent_file_node *) li->data);
    g_list_free(node->children);
    g_free(node->name);
    g_free(node);
}

void trg_torrent_file_free(trg_torrent_file * t)
{
    trg_torrent_file_node_free(t->top_node);
    g_free(t->name);
    g_free(t);
}

static trg_torrent_file_node *trg_parse_torrent_file_nodes(be_node *
                                                           info_node,
                                                           gint64 *
                                                           total_length)
{
    be_node *files_node = be_dict_find(info_node, "files", BE_LIST);
    trg_torrent_file_node *top_node = g_new0(trg_torrent_file_node, 1);
    int i;

    /* Probably means single file mode. */
    if (!files_node)
        return NULL;

    for (i = 0; files_node->val.l[i]; ++i) {
        be_node *file_node = files_node->val.l[i];

        if (be_validate_node(file_node, BE_DICT) ||
            !trg_torrent_file_node_insert(top_node, file_node, i,
                                          total_length)) {
            /* Unexpected format. Throw away everything, file indexes need to
             * be correct. */
            trg_torrent_file_node_free(top_node);
            return NULL;
        }
    }

    return top_node;
}

trg_torrent_file *trg_parse_torrent_file(char *filename)
{
    int fd;
    struct stat sb;
    void *addr;
    be_node *top_node, *info_node, *name_node;
    trg_torrent_file *ret = NULL;

    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        my_print_errno("opening file");
        return NULL;
    }

    if (fstat(fd, &sb) == -1) {
        my_print_errno("on fstat");
        close(fd);
        return NULL;
    }

    addr = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED) {
        my_print_errno("on mmap");
        close(fd);
        return NULL;
    }

    top_node = be_decoden((char *) addr, sb.st_size);
    munmap(addr, sb.st_size);
    close(fd);

    if (!top_node) {
        return NULL;
    } else if (be_validate_node(top_node, BE_DICT)) {
        goto out;
    }

    info_node = be_dict_find(top_node, "info", BE_DICT);
    if (!info_node)
        goto out;

    name_node = be_dict_find(info_node, "name", BE_STR);
    if (!name_node)
        goto out;

    ret = g_new0(trg_torrent_file, 1);
    ret->name = g_strdup(name_node->val.s);

    ret->top_node =
        trg_parse_torrent_file_nodes(info_node, &(ret->total_length));
    if (!ret->top_node) {
        trg_torrent_file_node *file_node;
        be_node *length_node = be_dict_find(info_node, "length", BE_INT);

        if (!length_node) {
            g_free(ret);
            ret = NULL;
            goto out;
        }

        file_node = g_new0(trg_torrent_file_node, 1);
        file_node->length = ret->total_length =
            (gint64) (length_node->val.i);
        file_node->name = g_strdup(ret->name);
        ret->top_node = file_node;
    }

  out:
    be_free(top_node);
    return ret;
}