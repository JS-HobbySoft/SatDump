/*
 * Generated by asn1c-0.9.28 (http://lionet.info/asn1c)
 * From ASN.1 module "FANSACTwoWayDataLinkCommunications"
 * 	found in "../../../libacars.asn1/fans-cpdlc.asn1"
 * 	`asn1c -fcompound-names -fincludes-quoted -gen-PER`
 */

#ifndef	_FANSAltitudeRestriction_H_
#define	_FANSAltitudeRestriction_H_


#include "asn_application.h"

/* Including external dependencies */
#include "FANSAltitude.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FANSAltitudeRestriction */
typedef FANSAltitude_t	 FANSAltitudeRestriction_t;

/* Implementation */
extern asn_TYPE_descriptor_t asn_DEF_FANSAltitudeRestriction;
asn_struct_free_f FANSAltitudeRestriction_free;
asn_struct_print_f FANSAltitudeRestriction_print;
asn_constr_check_f FANSAltitudeRestriction_constraint;
ber_type_decoder_f FANSAltitudeRestriction_decode_ber;
der_type_encoder_f FANSAltitudeRestriction_encode_der;
xer_type_decoder_f FANSAltitudeRestriction_decode_xer;
xer_type_encoder_f FANSAltitudeRestriction_encode_xer;
per_type_decoder_f FANSAltitudeRestriction_decode_uper;
per_type_encoder_f FANSAltitudeRestriction_encode_uper;

#ifdef __cplusplus
}
#endif

#endif	/* _FANSAltitudeRestriction_H_ */
#include "asn_internal.h"