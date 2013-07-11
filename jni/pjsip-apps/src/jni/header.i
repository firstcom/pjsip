/* $Id$ */

%module (directors="1") pjsua

%include "enums.swg"
%include "../my_typemaps.i"

%header %{
    #include <pjsua-lib/pjsua.h>
%}

/* Strip "pjsua_" prefix from pjsua functions, for better compatibility with 
 * pjsip-jni & csipsimple.
 */
%rename("%(strip:[pjsua_])s", %$isfunction) "";

/* Map 'void *' simply as long, app can use this "long" as index of its real user data */
%apply long long { void * };

/* Handle void *[ANY], e.g: pjsip_tx_data::mod_data, pjsip_transaction::mod_data */
//%ignore pjsip_tx_data::mod_data;
//%ignore pjsip_transaction::mod_data;
%apply long long[ANY]   { void *[ANY] };

/* Map pj_bool_t */
%apply bool { pj_bool_t };

/* Map "int*" & "unsigned*" as input & output */
%apply unsigned	*INOUT  { unsigned * };
%apply int      *INOUT  { int * };

/* Map the following args as input & output */
%apply int      *INOUT	{ pj_stun_nat_type * };
%apply int      *INOUT	{ pjsip_status_code * };
%apply int[ANY]         { pjmedia_format_id dec_fmt_id[ANY] };
%apply pj_str_t *INOUT  { pj_str_t *p_contact };

/* Handle members typed array of pj_str_t */
MY_JAVA_MEMBER_ARRAY_OF_STR(pjsua_config, nameserver, nameserver_count)
MY_JAVA_MEMBER_ARRAY_OF_STR(pjsua_config, outbound_proxy, outbound_proxy_cnt)
MY_JAVA_MEMBER_ARRAY_OF_STR(pjsua_config, stun_srv, stun_srv_cnt)
MY_JAVA_MEMBER_ARRAY_OF_STR(pjsua_acc_config, proxy, proxy_cnt)
MY_JAVA_MEMBER_ARRAY_OF_STR(pjsua_config, stun_srv, stun_srv_cnt)
MY_JAVA_MEMBER_ARRAY_OF_STR(pjsua_config, stun_srv, stun_srv_cnt)
MY_JAVA_MEMBER_ARRAY_OF_STR(pjsip_generic_array_hdr, values, count)

/* Handle pointer-to-pointer-to-object as input & output */
MY_JAVA_CLASS_INOUT(pjmedia_port, p_port)
MY_JAVA_CLASS_INOUT(pjsip_tx_data, p_tdata)

/* Handle array of pj_ssl_cipher in pjsip_tls_setting. */
MY_JAVA_MEMBER_ARRAY_OF_ENUM(pjsip_tls_setting, pj_ssl_cipher, ciphers, ciphers_num)

/* Handle array of pointer in struct/class member */
MY_JAVA_MEMBER_ARRAY_OF_POINTER(pjsip_regc_cbparam, pjsip_contact_hdr, contact, contact_cnt)
MY_JAVA_MEMBER_ARRAY_OF_POINTER(pjmedia_sdp_session, pjmedia_sdp_media, media, media_count)
MY_JAVA_MEMBER_ARRAY_OF_POINTER(pjmedia_sdp_media, pjmedia_sdp_bandw, bandw, bandw_count)
MY_JAVA_MEMBER_ARRAY_OF_POINTER(pjmedia_sdp_media, pjmedia_sdp_attr, attr, attr_count)

/* C++ SWIG target doesn't support nested class (C version does though!).
 * This is minimal workaround, ignore nested class as if it is not there.
 * TODO: proper workaround will be moving out inner classes to global scope.
 */
#ifdef __cplusplus
    %nestedworkaround pjmedia_codec_fmtp::param;
    %nestedworkaround pjsip_cred_info::ext;
    %nestedworkaround pjsip_event::body;
#endif

%include "../callbacks.i"

/* Global constants */
#define PJ_SUCCESS  0

