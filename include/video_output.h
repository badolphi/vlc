/*******************************************************************************
 * video_output.h : video output thread
 * (c)1999 VideoLAN
 *******************************************************************************
 * This module describes the programming interface for video output threads.
 * It includes functions allowing to open a new thread, send pictures to a
 * thread, and destroy a previously oppenned video output thread.
 *******************************************************************************/

/*******************************************************************************
 * vout_tables_t: pre-calculated convertion tables
 *******************************************************************************
 * These tables are used by convertion and scaling functions.
 *******************************************************************************/
typedef struct vout_tables_s
{
    void *              p_base;             /* base for all translation tables */    
    union 
    {        
        struct { u16 *p_red, *p_green, *p_blue; } rgb16;   /* color 15, 16 bpp */
        struct { u32 *p_red, *p_green, *p_blue; } rgb32;   /* color 24, 32 bpp */
        struct { u16 *p_gray; }                   gray16;   /* gray 15, 16 bpp */
        struct { u32 *p_gray; }                   gray32;   /* gray 24, 32 bpp */
    } yuv;    
    void *              p_trans_optimized;     /* optimized (all colors) */      
} vout_tables_t;

/*******************************************************************************
 * vout_convert_t: convertion function
 *******************************************************************************
 * This is the prototype common to all convertion functions. The type of p_pic
 * will change depending of the screen depth treated.
 * Parameters:
 *      p_vout                  video output thread
 *      p_pic                   picture address (start address in picture)
 *      p_y, p_u, p_v           Y,U,V samples addresses
 *      i_width                 Y samples width
 *      i_height                Y samples height
 *      i_eol                   number of Y samples to reach the next line 
 *      i_pic_eol               number or pixels to reach the next line
 *      i_scale                 if non 0, vertical scaling is 1 - 1/i_scale
 * Conditions:
 *      start x + i_width                        <  picture width
 *      start y + i_height * (scaling factor)    <  picture height
 *      i_width % 16                             == 0
 *******************************************************************************/
typedef void (vout_convert_t)( p_vout_thread_t p_vout, void *p_pic,
                               yuv_data_t *p_y, yuv_data_t *p_u, yuv_data_t *p_v,
                               int i_width, int i_height, int i_eol, int i_pic_eol,
                               int i_scale );

/*******************************************************************************
 * vout_scale_t: scaling function
 *******************************************************************************
 * When a picture can't be scaled unsing the fast i_y_scale parameter of a
 * transformation, it is rendered in a temporary buffer then scaled using a
 * totally accurate (but also very slow) method.
 * This is the prototype common to all scaling functions. The types of p_buffer
 * and p_pic will change depending of the screen depth treated.
 * Parameters:
 *      p_vout                  video output thread
 *      p_pic                   picture address (start address in picture)
 *      p_buffer                source picture
 *      i_width                 buffer width
 *      i_height                buffer height
 *      i_eol                   number of pixels to reach next buffer line
 *      i_pic_eol               number of pixels to reach next picture line
 *      f_alpha, f_beta         horizontal and vertical scaling factors
 *******************************************************************************/
typedef void (vout_scale_t)( p_vout_thread_t p_vout, void *p_pic, void *p_buffer, 
                             int i_width, int i_height, int i_eol, int i_pic_eol,
                             float f_alpha, float f_beta );

/*******************************************************************************
 * vout_thread_t: video output thread descriptor
 *******************************************************************************
 * Any independant video output device, such as an X11 window or a GGI device,
 * is represented by a video output thread, and described using following 
 * structure.
 *******************************************************************************/
typedef struct vout_thread_s
{
    /* Thread properties and lock */
    boolean_t           b_die;                                   /* `die' flag */
    boolean_t           b_error;                               /* `error' flag */
    boolean_t           b_active;                             /* `active' flag */
    pthread_t           thread_id;                 /* id for pthread functions */
    pthread_mutex_t     lock;                                   /* thread lock */
    int *               pi_status;                    /* temporary status flag */
    p_vout_sys_t        p_sys;                         /* system output method */

    /* Current display properties */
    boolean_t           b_info;              /* print additionnal informations */    
    boolean_t           b_grayscale;             /* color or grayscale display */    
    int                 i_width;                /* current output method width */
    int                 i_height;              /* current output method height */
    int                 i_bytes_per_line;/* bytes per line (including virtual) */    
    int                 i_screen_depth;              /* bits per pixel - FIXED */
    int                 i_bytes_per_pixel;        /* real screen depth - FIXED */
    float               f_x_ratio;                 /* horizontal display ratio */
    float               f_y_ratio;                   /* vertical display ratio */
    float               f_gamma;                                      /* gamma */    

    /* Changed properties values - some of them are treated directly by the
     * thread, the over may be ignored or handled by vout_SysManage */
    //?? info, grayscale, width, height, bytes per line, x ratio, y ratio, gamma
    boolean_t           b_gamma_change;              /* gamma change indicator */    
    int                 i_new_width;                              /* new width */    
    int                 i_new_height;                            /* new height */    

#ifdef STATS    
    /* Statistics - these numbers are not supposed to be accurate, but are a
     * good indication of the thread status */
    count_t             c_loops;                            /* number of loops */
    count_t             c_idle_loops;                  /* number of idle loops */
    count_t             c_fps_samples;                       /* picture counts */    
    mtime_t             fps_sample[ VOUT_FPS_SAMPLES ];   /* FPS samples dates */
#endif

#ifdef DEBUG_VIDEO
    /* Additionnal video debugging informations */
    mtime_t             picture_render_time;    /* last picture rendering time */
#endif
 
    /* Video heap and translation tables */
    picture_t           p_picture[VOUT_MAX_PICTURES];              /* pictures */
    vout_tables_t       tables;                          /* translation tables */
    vout_convert_t *    p_ConvertYUV420;                /* YUV 4:2:0 converter */
    vout_convert_t *    p_ConvertYUV422;                /* YUV 4:2:2 converter */
    vout_convert_t *    p_ConvertYUV444;                /* YUV 4:4:4 converter */
    vout_scale_t *      p_Scale;                                     /* scaler */
} vout_thread_t;

/*******************************************************************************
 * Prototypes
 *******************************************************************************/
vout_thread_t * vout_CreateThread               ( 
#ifdef VIDEO_X11
                                                  char *psz_display, Window root_window, 
#endif
                                                  int i_width, int i_height, int *pi_status
                                                );

void            vout_DestroyThread              ( vout_thread_t *p_vout, int *pi_status );

picture_t *     vout_CreatePicture              ( vout_thread_t *p_vout, int i_type, 
						  int i_width, int i_height );
void            vout_DestroyPicture             ( vout_thread_t *p_vout, picture_t *p_pic );
void            vout_DisplayPicture             ( vout_thread_t *p_vout, picture_t *p_pic );
void            vout_LinkPicture                ( vout_thread_t *p_vout, picture_t *p_pic );
void            vout_UnlinkPicture              ( vout_thread_t *p_vout, picture_t *p_pic );

















