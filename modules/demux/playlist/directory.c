/*****************************************************************************
 * directory.c : Use access readdir to output folder content to playlist
 *****************************************************************************
 * Copyright (C) 2014 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Julien 'Lta' BALLET <contact # lta . io >
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <ctype.h>

#include <vlc_common.h>
#include <vlc_demux.h>

#include "playlist.h"

struct demux_sys_t
{
    bool b_dir_sorted;
    bool b_dir_can_loop;
};

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int Demux( demux_t *p_demux );

static const char *const ppsz_sub_exts[] = {
    "idx", "sub",  "srt",
    "ssa", "ass",  "smi",
    "utf", "utf8", "utf-8",
    "rt",   "aqt", "txt",
    "usf", "jss",  "cdg",
    "psb", "mpsub","mpl2",
    "pjs", "dks", "stl",
    "vtt", "sbv",
    NULL,
};

static const char *const ppsz_audio_exts[] = {
    "ac3",
    NULL,
};

static struct {
    int i_type;
    const char *const *ppsz_exts;
} p_slave_list[] = {
    { SLAVE_TYPE_SPU, ppsz_sub_exts },
    { SLAVE_TYPE_AUDIO, ppsz_audio_exts },
};
#define SLAVE_TYPE_COUNT (sizeof(p_slave_list) / sizeof(*p_slave_list))

typedef struct {
    int                 i_slaves;
    input_item_slave  **pp_slaves;
} input_item_slave_list;

int Import_Dir ( vlc_object_t *p_this)
{
    demux_t  *p_demux = (demux_t *)p_this;
    bool b_dir_sorted, b_dir_can_loop;

    if( stream_Control( p_demux->s, STREAM_IS_DIRECTORY,
                        &b_dir_sorted, &b_dir_can_loop ) )
        return VLC_EGENERIC;

    STANDARD_DEMUX_INIT_MSG( "reading directory content" );
    p_demux->p_sys->b_dir_sorted = b_dir_sorted;
    p_demux->p_sys->b_dir_can_loop = b_dir_can_loop;

    return VLC_SUCCESS;
}

void Close_Dir ( vlc_object_t *p_this )
{
    demux_t *p_demux = (demux_t *)p_this;
    free( p_demux->p_sys );
}

/**
 * Does the provided URI/path/stuff has one of the extension provided ?
 *
 * \param psz_exts A comma separated list of extension without dot, or only
 * one ext (ex: "avi,mkv,webm")
 * \param psz_uri The uri/path to check (ex: "file:///home/foo/bar.avi"). If
 * providing an URI, it must not contain a query string.
 *
 * \return true if the uri/path has one of the provided extension
 * false otherwise.
 */
static bool has_ext( const char *psz_exts, const char *psz_uri )
{
    if( psz_exts == NULL )
        return false;

    const char *ext = strrchr( psz_uri, '.' );
    if( ext == NULL )
        return false;

    size_t extlen = strlen( ++ext );

    for( const char *type = psz_exts, *end; type[0]; type = end + 1 )
    {
        end = strchr( type, ',' );
        if( end == NULL )
            end = type + strlen( type );

        if( type + extlen == end && !strncasecmp( ext, type, extlen ) )
            return true;

        if( *end == '\0' )
            break;
    }

    return false;
}

static bool is_slave( const char *psz_name, int *p_slave_type )
{
    const char *psz_ext = strrchr( psz_name, '.' );
    if( psz_ext == NULL )
        return false;

    size_t i_extlen = strlen( ++psz_ext );
    if( i_extlen == 0 )
        return false;

    for( unsigned int i = 0; i < SLAVE_TYPE_COUNT; ++i )
    {
        for( const char *const *ppsz_slave_ext = p_slave_list[i].ppsz_exts;
             *ppsz_slave_ext != NULL; ppsz_slave_ext++ )
        {
            if( !strncasecmp( psz_ext, *ppsz_slave_ext, i_extlen ) )
            {
                *p_slave_type = p_slave_list[i].i_type;
                return true;
            }
        }
    }
    return false;
}

static int compar_type( input_item_t *p1, input_item_t *p2 )
{
    if( p1->i_type != p2->i_type )
    {
        if( p1->i_type == ITEM_TYPE_DIRECTORY )
            return -1;
        if( p2->i_type == ITEM_TYPE_DIRECTORY )
            return 1;
    }
    return 0;
}

static int compar_collate( input_item_t *p1, input_item_t *p2 )
{
    int i_ret = compar_type( p1, p2 );

    if( i_ret != 0 )
        return i_ret;

#ifdef HAVE_STRCOLL
    /* The program's LOCAL defines if case is ignored */
    return strcoll( p1->psz_name, p2->psz_name );
#else
    return strcasecmp( p1->psz_name, p2->psz_name );
#endif
}

static int compar_version( input_item_t *p1, input_item_t *p2 )
{
    int i_ret = compar_type( p1, p2 );

    if( i_ret != 0 )
        return i_ret;

    return strverscmp( p1->psz_name, p2->psz_name );
}

static char *name_from_uri(const char * psz_uri)
{
    char *psz_filename = strdup( psz_uri );
    if( !psz_filename )
        return NULL;

    char *psz_ptr = psz_filename;

    /* remove folders */
    char *psz_slash = strrchr( psz_ptr, '/' );
    if( psz_slash )
        psz_ptr = psz_slash + 1;

    /* remove extension */
    char *psz_dot = strrchr( psz_ptr, '.' );
    if( psz_dot )
        *psz_dot = '\0';

    /* remove leading white spaces */
    while( *psz_ptr != '\0' && *psz_ptr == ' ' )
        psz_ptr++;

    /* remove trailing white spaces */
    int i = strlen( psz_ptr ) - 1;
    while( psz_ptr[i] == ' ' && i >= 0)
        psz_ptr[i--] = '\0';

    /* convert to lower case */
    char *p = psz_ptr;
    while( *p != '\0' )
    {
        *p = tolower(*p);
        p++;
    }

    psz_ptr = strdup( psz_ptr );
    free( psz_filename );
    return psz_ptr;
}

static uint8_t calculate_slave_priority(input_item_t *p_item, input_item_slave *p_slave)
{
    char *psz_item_name = name_from_uri( p_item->psz_uri );
    char *psz_slave_name = name_from_uri( p_slave->psz_uri );

    if( !psz_item_name || !psz_slave_name)
    {
        p_slave->i_priority = SLAVE_PRIORITY_NONE;
        goto done;
    }

    /* check if the names match exactly */
    if( !strcmp( psz_item_name, psz_slave_name ) )
    {
        p_slave->i_priority = SLAVE_PRIORITY_MATCH_ALL;
        goto done;
    }

    /* check if the item name is a substring of the slave name */
    const char *psz_sub = strstr( psz_slave_name, psz_item_name );

    if( psz_sub )
    {
        /* check if the item name was found at the end of the slave name */
        if( strlen( psz_sub + strlen( psz_item_name ) ) == 0 )
        {
            p_slave->i_priority = SLAVE_PRIORITY_MATCH_RIGHT;
            goto done;
        }
        else
        {
            p_slave->i_priority = SLAVE_PRIORITY_MATCH_LEFT;
            goto done;
        }
    }
    p_slave->i_priority = SLAVE_PRIORITY_MATCH_NONE;
done:
    free( psz_item_name );
    free( psz_slave_name );
    return p_slave->i_priority;
}

static void attach_slaves( demux_t * p_demux, input_item_node_t *p_node,
                           input_item_slave_list *p_slaves )
{
    int i_fuzzy = var_InheritInteger( p_demux, "sub-autodetect-fuzzy" );
    if ( i_fuzzy == 0 )
        return;
    for( int i = 0; i < p_node->i_children; i++ )
    {
        input_item_t *p_item = p_node->pp_children[i]->p_item;
        for( int j = 0; j < p_slaves->i_slaves; j++ )
        {
            input_item_slave *p_slave = p_slaves->pp_slaves[j];
            if( calculate_slave_priority( p_item, p_slave ) >= i_fuzzy )
                input_item_AddSlave( p_item, p_slave );
            else
                input_item_slave_Delete( p_slave );
        }
    }
}

static void slave_list_Init(input_item_slave_list *list)
{
    list->i_slaves = 0;
    list->pp_slaves = NULL;
}

static void slave_list_Clear(input_item_slave_list *p_list)
{
    for( int i = 0; i < p_list->i_slaves; i++ )
        input_item_slave_Delete( p_list->pp_slaves[i] );
    TAB_CLEAN( p_list->i_slaves, p_list->pp_slaves );
}

static void slave_list_AppendItem(input_item_slave_list *p_list, input_item_slave *p_slave)
{
    INSERT_ELEM( p_list->pp_slaves, p_list->i_slaves, p_list->i_slaves, p_slave );
}

static int Demux( demux_t *p_demux )
{
    int i_ret = VLC_SUCCESS;
    input_item_t *p_input;
    input_item_node_t *p_node = NULL;
    input_item_slave_list p_slaves;
    input_item_t *p_item;
    char *psz_ignored_exts;
    bool b_show_hiddenfiles;

    p_input = GetCurrentItem( p_demux );
    p_node = input_item_node_Create( p_input );
    if( p_node == NULL )
        return VLC_ENOMEM;
    p_node->b_can_loop = p_demux->p_sys->b_dir_can_loop;

    b_show_hiddenfiles = var_InheritBool( p_demux, "show-hiddenfiles" );
    psz_ignored_exts = var_InheritString( p_demux, "ignore-filetypes" );

    slave_list_Init( &p_slaves );

    while( !i_ret && ( p_item = stream_ReadDir( p_demux->s ) ) )
    {
        int i_name_len = p_item->psz_name ? strlen( p_item->psz_name ) : 0;

        /* skip null, "." and ".." and hidden files if option is activated */
        if( !i_name_len || strcmp( p_item->psz_name, "." ) == 0
         || strcmp( p_item->psz_name, ".." ) == 0
         || ( !b_show_hiddenfiles && p_item->psz_name[0] == '.' ) )
            goto skip_item;

        /* Find slaves */
        int i_slave_type;
        if( is_slave( p_item->psz_name, &i_slave_type ) )
        {
            input_item_slave *p_slave = input_item_slave_New( p_item->psz_uri,
                                                              i_slave_type,
                                                              SLAVE_PRIORITY_NONE );
            if( !p_slave )
                i_ret = VLC_ENOMEM;
            else
                slave_list_AppendItem( &p_slaves, p_slave );
            goto skip_item;
        }

        /* skip ignored files */
        if( has_ext( psz_ignored_exts, p_item->psz_name ) )
            goto skip_item;

        input_item_CopyOptions( p_node->p_item, p_item );
        if( !input_item_node_AppendItem( p_node, p_item ) )
            i_ret = VLC_ENOMEM;
skip_item:
        input_item_Release( p_item );
    }
    free( psz_ignored_exts );

    input_item_Release(p_input);

    if( i_ret )
    {
        msg_Warn( p_demux, "unable to read directory" );
        input_item_node_Delete( p_node );
        slave_list_Clear( &p_slaves );
        return i_ret;
    }

    attach_slaves( p_demux, p_node, &p_slaves );

    if( !p_demux->p_sys->b_dir_sorted )
    {
        input_item_compar_cb compar_cb = NULL;
        char *psz_sort = var_InheritString( p_demux, "directory-sort" );

        if( psz_sort )
        {
            if( !strcasecmp( psz_sort, "version" ) )
                compar_cb = compar_version;
            else if( strcasecmp( psz_sort, "none" ) )
                compar_cb = compar_collate;
        }
        free( psz_sort );
        if( compar_cb )
            input_item_node_Sort( p_node, compar_cb );
    }

    input_item_node_PostAndDelete( p_node );
    return VLC_SUCCESS;
}
