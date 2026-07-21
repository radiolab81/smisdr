/* -*- c++ -*- */
#ifndef INCLUDED_SMISDR_API_H
#define INCLUDED_SMISDR_API_H

#include <gnuradio/attributes.h>

#ifdef gnuradio_smisdr_EXPORTS
#define SMISDR_API __GR_ATTR_EXPORT
#else
#define SMISDR_API __GR_ATTR_IMPORT
#endif

#endif /* INCLUDED_SMISDR_API_H */
