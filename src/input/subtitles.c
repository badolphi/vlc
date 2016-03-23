/*****************************************************************************
 * subtitles.c : subtitles detection
 *****************************************************************************
 * Copyright (C) 2003-2009 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Derk-Jan Hartman <hartman at videolan.org>
 * This is adapted code from the GPL'ed MPlayer (http://mplayerhq.hu)
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

/**
 *  \file
 *  This file contains functions to dectect subtitle files.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <ctype.h> /* isalnum() */
#include <unistd.h>
#include <sys/stat.h>

#include <vlc_common.h>
#include <vlc_fs.h>
#include <vlc_url.h>

#include "input_internal.h"

/**
 * The possible extensions for subtitle files we support
 */
static const char sub_exts[][6] = {
    "idx", "sub",  "srt",
    "ssa", "ass",  "smi",
    "utf", "utf8", "utf-8",
    "rt",   "aqt", "txt",
    "usf", "jss",  "cdg",
    "psb", "mpsub","mpl2",
    "pjs", "dks", "stl",
    "vtt", "sbv", ""
};

static void strcpy_trim( char *d, const char *s )
{
    unsigned char c;

    /* skip leading whitespace */
    while( ((c = *s) != '\0') && !isalnum(c) )
    {
        s++;
    }
    for(;;)
    {
        /* copy word */
        while( ((c = *s) != '\0') && isalnum(c) )
        {
            *d = tolower(c);
            s++; d++;
        }
        if( *s == 0 ) break;
        /* trim excess whitespace */
        while( ((c = *s) != '\0') && !isalnum(c) )
        {
            s++;
        }
        if( *s == 0 ) break;
        *d++ = ' ';
    }
    *d = 0;
}

static void strcpy_strip_ext( char *d, const char *s )
{
    unsigned char c;

    const char *tmp = strrchr(s, '.');
    if( !tmp )
    {
        strcpy(d, s);
        return;
    }
    else
        strlcpy(d, s, tmp - s + 1 );
    while( (c = *d) != '\0' )
    {
        *d = tolower(c);
        d++;
    }
}

static void strcpy_get_ext( char *d, const char *s )
{
    const char *tmp = strrchr(s, '.');
    if( !tmp )
        strcpy(d, "");
    else
        strcpy( d, tmp + 1 );
}

static int whiteonly( const char *s )
{
    unsigned char c;

    while( (c = *s) != '\0' )
    {
        if( isalnum( c ) )
            return 0;
        s++;
    }
    return 1;
}

/*
 * Check if a file ends with a subtitle extension
 */
int subtitles_Filter( const char *psz_dir_content )
{
    const char *tmp = strrchr( psz_dir_content, '.');

    if( !tmp )
        return 0;
    tmp++;

    for( int i = 0; sub_exts[i][0]; i++ )
        if( strcasecmp( sub_exts[i], tmp ) == 0 )
            return 1;
    return 0;
}


/**
 * Convert a list of paths separated by ',' to a char**
 */
static char **paths_to_list( const char *psz_dir, char *psz_path )
{
    unsigned int i, k, i_nb_subdirs;
    char **subdirs; /* list of subdirectories to look in */
    char *psz_parser = psz_path;

    if( !psz_dir || !psz_path )
        return NULL;

    for( k = 0, i_nb_subdirs = 1; psz_path[k] != '\0'; k++ )
    {
        if( psz_path[k] == ',' )
            i_nb_subdirs++;
    }

    subdirs = calloc( i_nb_subdirs + 1, sizeof(char*) );
    if( !subdirs )
        return NULL;

    for( i = 0; psz_parser && *psz_parser != '\0' ; )
    {
        char *psz_subdir = psz_parser;
        psz_parser = strchr( psz_subdir, ',' );
        if( psz_parser )
        {
            *psz_parser++ = '\0';
            while( *psz_parser == ' ' )
                psz_parser++;
        }
        if( *psz_subdir == '\0' )
            continue;

        if( asprintf( &subdirs[i++], "%s%s",
                  psz_subdir[0] == '.' ? psz_dir : "",
                  psz_subdir ) == -1 )
            break;
    }
    subdirs[i] = NULL;

    return subdirs;
}

subtitle *subtitle_New( const char *psz_path, uint8_t i_priority )
{
    if( !psz_path )
        return NULL;

    subtitle *p_sub = malloc( sizeof( *p_sub ) );
    if( !p_sub )
        return NULL;

    p_sub->psz_path = strdup( psz_path );
    p_sub->i_priority = i_priority;
    p_sub->b_rejected = false;

    if( !p_sub->psz_path )
    {
        free( p_sub );
        return NULL;
    }
    return p_sub;
}

void subtitle_Delete( subtitle *p_sub )
{
    free( p_sub->psz_path );
    free( p_sub );
}

void subtitle_list_Init( subtitle_list *p_list )
{
    p_list->i_subtitles = 0;
    p_list->pp_subtitles = NULL;
}

void subtitle_list_Clear( subtitle_list *p_list )
{
    for( int i = 0; i < p_list->i_subtitles; i++ )
        subtitle_Delete( p_list->pp_subtitles[i] );
    TAB_CLEAN( p_list->i_subtitles, p_list->pp_subtitles );
}

void subtitle_list_AppendItem( subtitle_list *p_list, subtitle *p_subtitle )
{
    INSERT_ELEM( p_list->pp_subtitles, p_list->i_subtitles, p_list->i_subtitles,
                 p_subtitle );
}

static int subtitle_Compare( const void *a, const void *b )
{
    const subtitle *p_sub0 = a;
    const subtitle *p_sub1 = b;

    if( p_sub0->i_priority > p_sub1->i_priority )
        return -1;

    if( p_sub0->i_priority < p_sub1->i_priority )
        return 1;

#ifdef HAVE_STRCOLL
    return strcoll( p_sub0->psz_path, p_sub1->psz_path );
#else
    return strcmp( p_sub0->psz_path, p_sub1->psz_path );
#endif
}

void subtitle_list_Sort( subtitle_list *p_list )
{
    qsort( p_list->pp_subtitles, p_list->i_subtitles, sizeof(subtitle),
           subtitle_Compare );
}

/**
 * Detect subtitle files.
 *
 * When called this function will split up the psz_name string into a
 * directory, filename and extension. It then opens the directory
 * in which the file resides and tries to find possible matches of
 * subtitles files.
 *
 * \ingroup Demux
 * \param p_this the calling \ref input_thread_t
 * \param psz_path a list of subdirectories (separated by a ',') to look in.
 * \param psz_name_org the complete filename to base the search on.
 * \param p_result an initialized subtitle list to append detected subtitles to.
 * \return VLC_SUCCESS if ok
 */
int subtitles_Detect( input_thread_t *p_this, char *psz_path, const char *psz_name_org,
                      subtitle_list *p_result )
{
    int i_fuzzy = var_GetInteger( p_this, "sub-autodetect-fuzzy" );
    if ( i_fuzzy == 0 )
        return VLC_EGENERIC;
    int j, i_fname_len;
    char *f_fname_noext = NULL, *f_fname_trim = NULL;
    char **subdirs; /* list of subdirectories to look in */

    if( !psz_name_org )
        return VLC_EGENERIC;

    char *psz_fname = vlc_uri2path( psz_name_org );
    if( !psz_fname )
        return VLC_EGENERIC;

    /* extract filename & dirname from psz_fname */
    char *f_dir = strdup( psz_fname );
    if( f_dir == 0 )
    {
        free( psz_fname );
        return VLC_ENOMEM;
    }

    const char *f_fname = strrchr( psz_fname, DIR_SEP_CHAR );
    if( !f_fname )
    {
        free( f_dir );
        free( psz_fname );
        return VLC_EGENERIC;
    }
    f_fname++; /* Skip the '/' */
    f_dir[f_fname - psz_fname] = 0; /* keep dir separator in f_dir */

    i_fname_len = strlen( f_fname );

    f_fname_noext = malloc(i_fname_len + 1);
    f_fname_trim = malloc(i_fname_len + 1 );
    if( !f_fname_noext || !f_fname_trim )
    {
        free( f_dir );
        free( f_fname_noext );
        free( f_fname_trim );
        free( psz_fname );
        return VLC_ENOMEM;
    }

    strcpy_strip_ext( f_fname_noext, f_fname );
    strcpy_trim( f_fname_trim, f_fname_noext );

    subdirs = paths_to_list( f_dir, psz_path );
    for( j = -1; (j == -1) || ( j >= 0 && subdirs != NULL && subdirs[j] != NULL ); j++ )
    {
        const char *psz_dir = (j < 0) ? f_dir : subdirs[j];
        if( psz_dir == NULL || ( j >= 0 && !strcmp( psz_dir, f_dir ) ) )
            continue;

        /* parse psz_src dir */
        DIR *dir = vlc_opendir( psz_dir );
        if( dir == NULL )
            continue;

        msg_Dbg( p_this, "looking for a subtitle file in %s", psz_dir );

        const char *psz_name;
        while( (psz_name = vlc_readdir( dir )) )
        {
            if( psz_name[0] == '.' || !subtitles_Filter( psz_name ) )
                continue;

            char tmp_fname_noext[strlen( psz_name ) + 1];
            char tmp_fname_trim[strlen( psz_name ) + 1];
            char tmp_fname_ext[strlen( psz_name ) + 1];
            const char *tmp;
            int i_prio = SLAVE_PRIORITY_NONE;

            /* retrieve various parts of the filename */
            strcpy_strip_ext( tmp_fname_noext, psz_name );
            strcpy_get_ext( tmp_fname_ext, psz_name );
            strcpy_trim( tmp_fname_trim, tmp_fname_noext );

            if( !strcmp( tmp_fname_trim, f_fname_trim ) )
            {
                /* matches the movie name exactly */
                i_prio = SLAVE_PRIORITY_MATCH_ALL;
            }
            else if( (tmp = strstr( tmp_fname_trim, f_fname_trim )) )
            {
                /* contains the movie name */
                tmp += strlen( f_fname_trim );
                if( whiteonly( tmp ) )
                {
                    /* chars in front of the movie name */
                    i_prio = SLAVE_PRIORITY_MATCH_RIGHT;
                }
                else
                {
                    /* chars after (and possibly in front of)
                     * the movie name */
                    i_prio = SLAVE_PRIORITY_MATCH_LEFT;
                }
            }
            else if( j == -1 )
            {
                /* doesn't contain the movie name, prefer files in f_dir over subdirs */
                i_prio = SLAVE_PRIORITY_MATCH_NONE;
            }
            if( i_prio >= i_fuzzy )
            {
                struct stat st;
                char *path;

                if( asprintf( &path, "%s"DIR_SEP"%s", psz_dir, psz_name ) < 0 )
                    continue;

                if( strcmp( path, psz_fname )
                 && vlc_stat( path, &st ) == 0
                 && S_ISREG( st.st_mode ) )
                {
                    msg_Dbg( p_this,
                            "autodetected subtitle: %s with priority %d",
                            path, i_prio );
                    subtitle *p_sub = subtitle_New( path, i_prio );
                    subtitle_list_AppendItem( p_result, p_sub );
                }
                free( path );
            }
        }
        closedir( dir );
    }
    if( subdirs )
    {
        for( j = 0; subdirs[j]; j++ )
            free( subdirs[j] );
        free( subdirs );
    }
    free( f_dir );
    free( f_fname_trim );
    free( f_fname_noext );
    free( psz_fname );

    for( int i = 0; i < p_result->i_subtitles; i++ )
    {
        subtitle *p_sub = p_result->pp_subtitles[i];

        if( !p_sub->psz_path )
        {
            p_sub->b_rejected = true;
            continue;
        }

        char *psz_ext = strrchr( p_sub->psz_path, '.' );
        if( !psz_ext )
            continue;
        psz_ext++;

        if( !strcasecmp( psz_ext, "sub" ) )
        {
            for( int j = 0; i < p_result->i_subtitles; j++ )
            {
                subtitle *p_sub_inner = p_result->pp_subtitles[j];

                /* check that the filenames without extension match */
                if( strncasecmp( p_sub->psz_path, p_sub_inner->psz_path,
                    strlen( p_sub->psz_path ) - 3 ) )
                    continue;

                char *psz_ext_inner = strrchr( p_sub_inner->psz_path, '.' );
                if( !psz_ext_inner )
                    continue;
                psz_ext_inner++;

                /* check that we have an idx file */
                if( !strcasecmp( psz_ext_inner, "idx" ) )
                {
                    p_sub->b_rejected = true;
                    break;
                }
            }
        }
        else if( !strcasecmp( psz_ext, "cdg" ) )
        {
            if( p_sub->i_priority < SLAVE_PRIORITY_MATCH_ALL )
                p_sub->b_rejected = true;
        }
    }
    return VLC_SUCCESS;
}
