/*****************************************************************************
 * ts_psi_eit.c : TS demuxer EIT handling
 *****************************************************************************
 * Copyright (C) 2014-2016 - VideoLAN Authors
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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_demux.h>
#include <vlc_meta.h>
#include <vlc_epg.h>
#include <vlc_charset.h>   /* FromCharset, for EIT */

#ifndef _DVBPSI_DVBPSI_H_
 #include <dvbpsi/dvbpsi.h>
#endif
#include <dvbpsi/demux.h>
#include <dvbpsi/descriptor.h>
#include <dvbpsi/sdt.h>
#include <dvbpsi/eit.h> /* EIT support */
#include <dvbpsi/tot.h> /* TDT support */
#include <dvbpsi/dr.h>

#include "ts_psi_eit.h"

#include "ts_pid.h"
#include "ts_streams_private.h"
#include "ts.h"
#include "../dvb-text.h"

#ifdef HAVE_ARIBB24
 #include <aribb24/decoder.h>
#endif

#include <time.h>
#include <assert.h>

static char *EITConvertToUTF8( demux_t *p_demux,
                               const unsigned char *psz_instring,
                               size_t i_length,
                               bool b_broken )
{
    demux_sys_t *p_sys = p_demux->p_sys;
#ifdef HAVE_ARIBB24
    if( p_sys->arib.e_mode == ARIBMODE_ENABLED )
    {
        if ( !p_sys->arib.p_instance )
            p_sys->arib.p_instance = arib_instance_new( p_demux );
        if ( !p_sys->arib.p_instance )
            return NULL;
        arib_decoder_t *p_decoder = arib_get_decoder( p_sys->arib.p_instance );
        if ( !p_decoder )
            return NULL;

        char *psz_outstring = NULL;
        size_t i_out;

        i_out = i_length * 4;
        psz_outstring = (char*) calloc( i_out + 1, sizeof(char) );
        if( !psz_outstring )
            return NULL;

        arib_initialize_decoder( p_decoder );
        i_out = arib_decode_buffer( p_decoder, psz_instring, i_length,
                                    psz_outstring, i_out );
        arib_finalize_decoder( p_decoder );

        return psz_outstring;
    }
#else
    VLC_UNUSED(p_sys);
#endif
    /* Deal with no longer broken providers (no switch byte
      but sending ISO_8859-1 instead of ISO_6937) without
      removing them from the broken providers table
      (keep the entry for correctly handling recorded TS).
    */
    b_broken = b_broken && i_length && *psz_instring > 0x20;

    if( b_broken )
        return FromCharset( "ISO_8859-1", psz_instring, i_length );
    return vlc_from_EIT( psz_instring, i_length );
}

static void SDTCallBack( demux_t *p_demux, dvbpsi_sdt_t *p_sdt )
{
    demux_sys_t          *p_sys = p_demux->p_sys;
    ts_pid_t             *sdt = GetPID(p_sys, 0x11);
    dvbpsi_sdt_service_t *p_srv;

    msg_Dbg( p_demux, "SDTCallBack called" );

    if( p_sys->es_creation != CREATE_ES ||
       !p_sdt->b_current_next ||
        p_sdt->i_version == sdt->u.p_psi->i_version )
    {
        dvbpsi_sdt_delete( p_sdt );
        return;
    }

    msg_Dbg( p_demux, "new SDT ts_id=%d version=%d current_next=%d "
             "network_id=%d",
             p_sdt->i_extension,
             p_sdt->i_version, p_sdt->b_current_next,
             p_sdt->i_network_id );

    p_sys->b_broken_charset = false;

    for( p_srv = p_sdt->p_first_service; p_srv; p_srv = p_srv->p_next )
    {
        vlc_meta_t          *p_meta;
        dvbpsi_descriptor_t *p_dr;

        const char *psz_type = NULL;
        const char *psz_status = NULL;

        msg_Dbg( p_demux, "  * service id=%d eit schedule=%d present=%d "
                 "running=%d free_ca=%d",
                 p_srv->i_service_id, p_srv->b_eit_schedule,
                 p_srv->b_eit_present, p_srv->i_running_status,
                 p_srv->b_free_ca );

        if( p_sys->vdr.i_service && p_srv->i_service_id != p_sys->vdr.i_service )
        {
            msg_Dbg( p_demux, "  * service id=%d skipped (not declared in vdr header)",
                     p_sys->vdr.i_service );
            continue;
        }

        p_meta = vlc_meta_New();
        for( p_dr = p_srv->p_first_descriptor; p_dr; p_dr = p_dr->p_next )
        {
            if( p_dr->i_tag == 0x48 )
            {
                static const char *ppsz_type[17] = {
                    "Reserved",
                    "Digital television service",
                    "Digital radio sound service",
                    "Teletext service",
                    "NVOD reference service",
                    "NVOD time-shifted service",
                    "Mosaic service",
                    "PAL coded signal",
                    "SECAM coded signal",
                    "D/D2-MAC",
                    "FM Radio",
                    "NTSC coded signal",
                    "Data broadcast service",
                    "Reserved for Common Interface Usage",
                    "RCS Map (see EN 301 790 [35])",
                    "RCS FLS (see EN 301 790 [35])",
                    "DVB MHP service"
                };
                dvbpsi_service_dr_t *pD = dvbpsi_DecodeServiceDr( p_dr );
                char *str1 = NULL;
                char *str2 = NULL;

                /* Workarounds for broadcasters with broken EPG */

                if( p_sdt->i_network_id == 133 )
                    p_sys->b_broken_charset = true;  /* SKY DE & BetaDigital use ISO8859-1 */

                /* List of providers using ISO8859-1 */
                static const char ppsz_broken_providers[][8] = {
                    "CSAT",     /* CanalSat FR */
                    "GR1",      /* France televisions */
                    "MULTI4",   /* NT1 */
                    "MR5",      /* France 2/M6 HD */
                    ""
                };
                for( int i = 0; *ppsz_broken_providers[i]; i++ )
                {
                    const size_t i_length = strlen(ppsz_broken_providers[i]);
                    if( pD->i_service_provider_name_length == i_length &&
                        !strncmp( (char *)pD->i_service_provider_name, ppsz_broken_providers[i], i_length ) )
                        p_sys->b_broken_charset = true;
                }

                /* FIXME: Digital+ ES also uses ISO8859-1 */

                str1 = EITConvertToUTF8(p_demux,
                                        pD->i_service_provider_name,
                                        pD->i_service_provider_name_length,
                                        p_sys->b_broken_charset );
                str2 = EITConvertToUTF8(p_demux,
                                        pD->i_service_name,
                                        pD->i_service_name_length,
                                        p_sys->b_broken_charset );

                msg_Dbg( p_demux, "    - type=%d provider=%s name=%s",
                         pD->i_service_type, str1, str2 );

                vlc_meta_SetTitle( p_meta, str2 );
                vlc_meta_SetPublisher( p_meta, str1 );
                if( pD->i_service_type >= 0x01 && pD->i_service_type <= 0x10 )
                    psz_type = ppsz_type[pD->i_service_type];
                free( str1 );
                free( str2 );
            }
        }

        if( p_srv->i_running_status >= 0x01 && p_srv->i_running_status <= 0x04 )
        {
            static const char *ppsz_status[5] = {
                "Unknown",
                "Not running",
                "Starts in a few seconds",
                "Pausing",
                "Running"
            };
            psz_status = ppsz_status[p_srv->i_running_status];
        }

        if( psz_type )
            vlc_meta_AddExtra( p_meta, "Type", psz_type );
        if( psz_status )
            vlc_meta_AddExtra( p_meta, "Status", psz_status );

        es_out_Control( p_demux->out, ES_OUT_SET_GROUP_META,
                        p_srv->i_service_id, p_meta );
        vlc_meta_Delete( p_meta );
    }

    sdt->u.p_psi->i_version = p_sdt->i_version;
    dvbpsi_sdt_delete( p_sdt );
}

static void EITDecodeMjd( int i_mjd, int *p_y, int *p_m, int *p_d )
{
    const int yp = (int)( ( (double)i_mjd - 15078.2)/365.25 );
    const int mp = (int)( ((double)i_mjd - 14956.1 - (int)(yp * 365.25)) / 30.6001 );
    const int c = ( mp == 14 || mp == 15 ) ? 1 : 0;

    *p_y = 1900 + yp + c*1;
    *p_m = mp - 1 - c*12;
    *p_d = i_mjd - 14956 - (int)(yp*365.25) - (int)(mp*30.6001);
}
#define CVT_FROM_BCD(v) ((((v) >> 4)&0xf)*10 + ((v)&0xf))
static int64_t EITConvertStartTime( uint64_t i_date )
{
    const int i_mjd = i_date >> 24;
    struct tm tm;

    tm.tm_hour = CVT_FROM_BCD(i_date >> 16);
    tm.tm_min  = CVT_FROM_BCD(i_date >>  8);
    tm.tm_sec  = CVT_FROM_BCD(i_date      );

    /* if all 40 bits are 1, the start is unknown */
    if( i_date == UINT64_C(0xffffffffff) )
        return -1;

    EITDecodeMjd( i_mjd, &tm.tm_year, &tm.tm_mon, &tm.tm_mday );
    tm.tm_year -= 1900;
    tm.tm_mon--;
    tm.tm_isdst = 0;

    return timegm( &tm );
}
static int EITConvertDuration( uint32_t i_duration )
{
    return CVT_FROM_BCD(i_duration >> 16) * 3600 +
           CVT_FROM_BCD(i_duration >> 8 ) * 60 +
           CVT_FROM_BCD(i_duration      );
}
#undef CVT_FROM_BCD

static void TDTCallBack( demux_t *p_demux, dvbpsi_tot_t *p_tdt )
{
    demux_sys_t        *p_sys = p_demux->p_sys;

    p_sys->i_tdt_delta = CLOCK_FREQ * EITConvertStartTime( p_tdt->i_utc_time )
                         - mdate();
    dvbpsi_tot_delete(p_tdt);
}


static void EITCallBack( demux_t *p_demux,
                         dvbpsi_eit_t *p_eit, bool b_current_following )
{
    demux_sys_t        *p_sys = p_demux->p_sys;
    dvbpsi_eit_event_t *p_evt;
    vlc_epg_t *p_epg;

    msg_Dbg( p_demux, "EITCallBack called" );
    if( !p_eit->b_current_next )
    {
        dvbpsi_eit_delete( p_eit );
        return;
    }

    msg_Dbg( p_demux, "new EIT service_id=%d version=%d current_next=%d "
             "ts_id=%d network_id=%d segment_last_section_number=%d "
             "last_table_id=%d",
             p_eit->i_extension,
             p_eit->i_version, p_eit->b_current_next,
             p_eit->i_ts_id, p_eit->i_network_id,
             p_eit->i_segment_last_section_number, p_eit->i_last_table_id );

    p_epg = vlc_epg_New( NULL );
    for( p_evt = p_eit->p_first_event; p_evt; p_evt = p_evt->p_next )
    {
        dvbpsi_descriptor_t *p_dr;
        char                *psz_name = NULL;
        char                *psz_text = NULL;
        char                *psz_extra = strdup("");
        int64_t i_start;
        int i_duration;
        int i_min_age = 0;
        int64_t i_tot_time = 0;

        i_start = EITConvertStartTime( p_evt->i_start_time );
        i_duration = EITConvertDuration( p_evt->i_duration );

        if( p_sys->arib.e_mode == ARIBMODE_ENABLED )
        {
            if( p_sys->i_tdt_delta == 0 )
                p_sys->i_tdt_delta = CLOCK_FREQ * (i_start + i_duration - 5) - mdate();

            i_tot_time = (mdate() + p_sys->i_tdt_delta) / CLOCK_FREQ;

            tzset(); // JST -> UTC
            i_start += timezone; // FIXME: what about DST?
            i_tot_time += timezone;

            if( p_evt->i_running_status == 0x00 &&
                (i_start - 5 < i_tot_time &&
                 i_tot_time < i_start + i_duration + 5) )
            {
                p_evt->i_running_status = 0x04;
                msg_Dbg( p_demux, "  EIT running status 0x00 -> 0x04" );
            }
        }

        msg_Dbg( p_demux, "  * event id=%d start_time:%d duration=%d "
                          "running=%d free_ca=%d",
                 p_evt->i_event_id, (int)i_start, (int)i_duration,
                 p_evt->i_running_status, p_evt->b_free_ca );

        for( p_dr = p_evt->p_first_descriptor; p_dr; p_dr = p_dr->p_next )
        {
            switch(p_dr->i_tag)
            {
            case 0x4d:
            {
                dvbpsi_short_event_dr_t *pE = dvbpsi_DecodeShortEventDr( p_dr );

                /* Only take first description, as we don't handle language-info
                   for epg atm*/
                if( pE && psz_name == NULL )
                {
                    psz_name = EITConvertToUTF8( p_demux,
                                                 pE->i_event_name, pE->i_event_name_length,
                                                 p_sys->b_broken_charset );
                    free( psz_text );
                    psz_text = EITConvertToUTF8( p_demux,
                                                 pE->i_text, pE->i_text_length,
                                                 p_sys->b_broken_charset );
                    msg_Dbg( p_demux, "    - short event lang=%3.3s '%s' : '%s'",
                             pE->i_iso_639_code, psz_name, psz_text );
                }
            }
                break;

            case 0x4e:
            {
                dvbpsi_extended_event_dr_t *pE = dvbpsi_DecodeExtendedEventDr( p_dr );
                if( pE )
                {
                    msg_Dbg( p_demux, "    - extended event lang=%3.3s [%d/%d]",
                             pE->i_iso_639_code,
                             pE->i_descriptor_number, pE->i_last_descriptor_number );

                    if( pE->i_text_length > 0 )
                    {
                        char *psz_text = EITConvertToUTF8( p_demux,
                                                           pE->i_text, pE->i_text_length,
                                                           p_sys->b_broken_charset );
                        if( psz_text )
                        {
                            msg_Dbg( p_demux, "       - text='%s'", psz_text );

                            psz_extra = xrealloc( psz_extra,
                                   strlen(psz_extra) + strlen(psz_text) + 1 );
                            strcat( psz_extra, psz_text );
                            free( psz_text );
                        }
                    }

                    for( int i = 0; i < pE->i_entry_count; i++ )
                    {
                        char *psz_dsc = EITConvertToUTF8( p_demux,
                                                          pE->i_item_description[i],
                                                          pE->i_item_description_length[i],
                                                          p_sys->b_broken_charset );
                        char *psz_itm = EITConvertToUTF8( p_demux,
                                                          pE->i_item[i], pE->i_item_length[i],
                                                          p_sys->b_broken_charset );

                        if( psz_dsc && psz_itm )
                        {
                            msg_Dbg( p_demux, "       - desc='%s' item='%s'", psz_dsc, psz_itm );
#if 0
                            psz_extra = xrealloc( psz_extra,
                                         strlen(psz_extra) + strlen(psz_dsc) +
                                         strlen(psz_itm) + 3 + 1 );
                            strcat( psz_extra, "(" );
                            strcat( psz_extra, psz_dsc );
                            strcat( psz_extra, " " );
                            strcat( psz_extra, psz_itm );
                            strcat( psz_extra, ")" );
#endif
                        }
                        free( psz_dsc );
                        free( psz_itm );
                    }
                }
            }
                break;

            case 0x55:
            {
                dvbpsi_parental_rating_dr_t *pR = dvbpsi_DecodeParentalRatingDr( p_dr );
                if ( pR )
                {
                    for ( int i = 0; i < pR->i_ratings_number; i++ )
                    {
                        const dvbpsi_parental_rating_t *p_rating = & pR->p_parental_rating[ i ];
                        if ( p_rating->i_rating > 0x00 && p_rating->i_rating <= 0x0F )
                        {
                            if ( p_rating->i_rating + 3 > i_min_age )
                                i_min_age = p_rating->i_rating + 3;
                            msg_Dbg( p_demux, "    - parental control set to %d years",
                                     i_min_age );
                        }
                    }
                }
            }
                break;

            default:
                msg_Dbg( p_demux, "    - event unknown dr 0x%x(%d)", p_dr->i_tag, p_dr->i_tag );
                break;
            }
        }

        /* */
        if( i_start > 0 && psz_name && psz_text)
            vlc_epg_AddEvent( p_epg, i_start, i_duration, psz_name, psz_text,
                              *psz_extra ? psz_extra : NULL, i_min_age );

        /* Update "now playing" field */
        if( p_evt->i_running_status == 0x04 && i_start > 0  && psz_name && psz_text )
            vlc_epg_SetCurrent( p_epg, i_start );

        free( psz_name );
        free( psz_text );

        free( psz_extra );
    }
    if( p_epg->i_event > 0 )
    {
        if( b_current_following &&
            (  p_sys->programs.i_size == 0 ||
               p_sys->programs.p_elems[0] ==
                    p_eit->i_extension
                ) )
        {
            p_sys->i_dvb_length = 0;
            p_sys->i_dvb_start = 0;

            if( p_epg->p_current )
            {
                p_sys->i_dvb_start = CLOCK_FREQ * p_epg->p_current->i_start;
                p_sys->i_dvb_length = CLOCK_FREQ * p_epg->p_current->i_duration;
            }
        }
        es_out_Control( p_demux->out, ES_OUT_SET_GROUP_EPG,
                        p_eit->i_extension,
                        p_epg );
    }
    vlc_epg_Delete( p_epg );

    dvbpsi_eit_delete( p_eit );
}
static void EITCallBackCurrentFollowing( demux_t *p_demux, dvbpsi_eit_t *p_eit )
{
    EITCallBack( p_demux, p_eit, true );
}
static void EITCallBackSchedule( demux_t *p_demux, dvbpsi_eit_t *p_eit )
{
    EITCallBack( p_demux, p_eit, false );
}

static void PSINewTableCallBack( dvbpsi_t *h, uint8_t i_table_id,
                                 uint16_t i_extension, demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    assert( h );
#if 0
    msg_Dbg( p_demux, "PSINewTableCallBack: table 0x%x(%d) ext=0x%x(%d)",
             i_table_id, i_table_id, i_extension, i_extension );
#endif
    if( GetPID(p_sys, 0)->u.p_pat->i_version != -1 && i_table_id == 0x42 )
    {
        msg_Dbg( p_demux, "PSINewTableCallBack: table 0x%x(%d) ext=0x%x(%d)",
                 i_table_id, i_table_id, i_extension, i_extension );

        if( !dvbpsi_sdt_attach( h, i_table_id, i_extension, (dvbpsi_sdt_callback)SDTCallBack, p_demux ) )
            msg_Err( p_demux, "PSINewTableCallback: failed attaching SDTCallback" );
    }
    else if( GetPID(p_sys, 0x11)->u.p_psi->i_version != -1 &&
             ( i_table_id == 0x4e || /* Current/Following */
               (i_table_id >= 0x50 && i_table_id <= 0x5f) ) ) /* Schedule */
    {
        msg_Dbg( p_demux, "PSINewTableCallBack: table 0x%x(%d) ext=0x%x(%d)",
                 i_table_id, i_table_id, i_extension, i_extension );

        dvbpsi_eit_callback cb = i_table_id == 0x4e ?
                                    (dvbpsi_eit_callback)EITCallBackCurrentFollowing :
                                    (dvbpsi_eit_callback)EITCallBackSchedule;

        if( !dvbpsi_eit_attach( h, i_table_id, i_extension, cb, p_demux ) )
            msg_Err( p_demux, "PSINewTableCallback: failed attaching EITCallback" );
    }
    else if( GetPID(p_sys, 0x11)->u.p_psi->i_version != -1 &&
            (i_table_id == 0x70 /* TDT */ || i_table_id == 0x73 /* TOT */) )
    {
         msg_Dbg( p_demux, "PSINewTableCallBack: table 0x%x(%d) ext=0x%x(%d)",
                 i_table_id, i_table_id, i_extension, i_extension );

        if( !dvbpsi_tot_attach( h, i_table_id, i_extension, (dvbpsi_tot_callback)TDTCallBack, p_demux ) )
            msg_Err( p_demux, "PSINewTableCallback: failed attaching TDTCallback" );
    }
}

bool AttachDvbpsiNewEITTableHandler( dvbpsi_t *p_handle, demux_t * p_demux )
{
    return dvbpsi_AttachDemux( p_handle, (dvbpsi_demux_new_cb_t)PSINewTableCallBack, p_demux );
}
