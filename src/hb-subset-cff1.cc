/*
 * Copyright © 2018 Adobe Systems Incorporated.
 *
 *  This is part of HarfBuzz, a text shaping library.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and its documentation for any purpose, provided that the
 * above copyright notice and the following two paragraphs appear in
 * all copies of this software.
 *
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN
 * IF THE COPYRIGHT HOLDER HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * THE COPYRIGHT HOLDER SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE COPYRIGHT HOLDER HAS NO OBLIGATION TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 * Adobe Author(s): Michiharu Ariza
 */

#include "hb-open-type.hh"
#include "hb-ot-cff1-table.hh"
#include "hb-set.h"
#include "hb-subset-cff1.hh"
#include "hb-subset-plan.hh"
#include "hb-subset-cff-common.hh"
#include "hb-cff1-interp-cs.hh"

using namespace CFF;

struct RemapSID : Remap
{
  inline unsigned int add (unsigned int sid)
  {
    if ((sid != CFF_UNDEF_SID) && !is_std_std (sid))
      return offset_sid (Remap::add (unoffset_sid (sid)));
    else
      return sid;
  }

  inline unsigned int operator[] (unsigned int sid) const
  {
    if (is_std_std (sid))
      return sid;
    else
      return offset_sid (Remap::operator [] (unoffset_sid (sid)));
  }

  static const unsigned int num_std_strings = 391;

  static inline bool is_std_std (unsigned int sid) { return sid < num_std_strings; }
  static inline unsigned int offset_sid (unsigned int sid) { return sid + num_std_strings; }
  static inline unsigned int unoffset_sid (unsigned int sid) { return sid - num_std_strings; }
};

struct CFF1SubTableOffsets : CFFSubTableOffsets
{
  inline CFF1SubTableOffsets (void)
    : CFFSubTableOffsets (),
      nameIndexOffset (0),
      encodingOffset (0)
  {
    stringIndexInfo.init ();
    charsetInfo.init ();
    privateDictInfo.init ();
  }

  unsigned int  nameIndexOffset;
  TableInfo     stringIndexInfo;
  unsigned int  encodingOffset;
  TableInfo     charsetInfo;
  TableInfo     privateDictInfo;
};

/* a copy of a parsed out CFF1TopDictValues augmented with additional operators */
struct CFF1TopDictValuesMod : CFF1TopDictValues
{
  inline void init (const CFF1TopDictValues *base_= &Null(CFF1TopDictValues))
  {
    SUPER::init ();
    base = base_;
  }

  inline void fini (void)
  {
    SUPER::fini ();
  }

  inline unsigned getNumValues (void) const
  {
    return base->getNumValues () + SUPER::getNumValues ();
  }
  inline const CFF1TopDictVal &getValue (unsigned int i) const
  {
    if (i < base->getNumValues ())
      return (*base)[i];
    else
      return SUPER::values[i - base->getNumValues ()];
  }
  inline const CFF1TopDictVal &operator [] (unsigned int i) const { return getValue (i); }

  inline void reassignSIDs (const RemapSID& sidmap)
  {
    for (unsigned int i = 0; i < NameDictValues::ValCount; i++)
      nameSIDs[i] = sidmap[base->nameSIDs[i]];
  }

  protected:
  typedef CFF1TopDictValues SUPER;
  const CFF1TopDictValues *base;
};

struct TopDictModifiers
{
  inline TopDictModifiers (const CFF1SubTableOffsets &offsets_,
                           const unsigned int (&nameSIDs_)[NameDictValues::ValCount])
    : offsets (offsets_),
      nameSIDs (nameSIDs_)
  {}

  const CFF1SubTableOffsets &offsets;
  const unsigned int        (&nameSIDs)[NameDictValues::ValCount];
};

struct CFF1TopDict_OpSerializer : CFFTopDict_OpSerializer<CFF1TopDictVal>
{
  inline bool serialize (hb_serialize_context_t *c,
                         const CFF1TopDictVal &opstr,
                         const TopDictModifiers &mod) const
  {
    TRACE_SERIALIZE (this);

    OpCode op = opstr.op;
    switch (op)
    {
      case OpCode_charset:
        return_trace (FontDict::serialize_offset4_op(c, op, mod.offsets.charsetInfo.offset));

      case OpCode_Encoding:
        return_trace (FontDict::serialize_offset4_op(c, op, mod.offsets.encodingOffset));

      case OpCode_Private:
        {
          if (unlikely (!UnsizedByteStr::serialize_int2 (c, mod.offsets.privateDictInfo.size)))
            return_trace (false);
          if (unlikely (!UnsizedByteStr::serialize_int4 (c, mod.offsets.privateDictInfo.offset)))
            return_trace (false);
          HBUINT8 *p = c->allocate_size<HBUINT8> (1);
          if (unlikely (p == nullptr)) return_trace (false);
          p->set (OpCode_Private);
        }
        break;

      case OpCode_version:
      case OpCode_Notice:
      case OpCode_Copyright:
      case OpCode_FullName:
      case OpCode_FamilyName:
      case OpCode_Weight:
      case OpCode_PostScript:
      case OpCode_BaseFontName:
      case OpCode_FontName:
        return_trace (FontDict::serialize_offset2_op(c, op, mod.nameSIDs[NameDictValues::name_op_to_index (op)]));

      case OpCode_ROS:
        {
          /* for registry & ordering, reassigned SIDs are serialized
           * for supplement, the original byte string is copied along with the op code */
          OpStr supp_op;
          supp_op.op = op;
          supp_op.str.str = opstr.str.str + opstr.last_arg_offset;
          assert (opstr.str.len >= opstr.last_arg_offset + 3);
          supp_op.str.len = opstr.str.len - opstr.last_arg_offset;
        return_trace (UnsizedByteStr::serialize_int2 (c, mod.nameSIDs[NameDictValues::registry]) &&
                      UnsizedByteStr::serialize_int2 (c, mod.nameSIDs[NameDictValues::ordering]) &&
                      copy_opstr (c, supp_op));
        }
      default:
        return_trace (CFFTopDict_OpSerializer<CFF1TopDictVal>::serialize (c, opstr, mod.offsets));
    }
    return_trace (true);
  }

  inline unsigned int calculate_serialized_size (const CFF1TopDictVal &opstr) const
  {
    OpCode op = opstr.op;
    switch (op)
    {
      case OpCode_charset:
      case OpCode_Encoding:
        return OpCode_Size (OpCode_longintdict) + 4 + OpCode_Size (op);
    
      case OpCode_Private:
        return OpCode_Size (OpCode_longintdict) + 4 + OpCode_Size (OpCode_shortint) + 2 + OpCode_Size (OpCode_Private);
    
      case OpCode_version:
      case OpCode_Notice:
      case OpCode_Copyright:
      case OpCode_FullName:
      case OpCode_FamilyName:
      case OpCode_Weight:
      case OpCode_PostScript:
      case OpCode_BaseFontName:
      case OpCode_FontName:
        return OpCode_Size (OpCode_shortint) + 2 + OpCode_Size (op);

      case OpCode_ROS:
        return ((OpCode_Size (OpCode_shortint) + 2) * 2) + (opstr.str.len - opstr.last_arg_offset)/* supplement + op */;

      default:
        return CFFTopDict_OpSerializer<CFF1TopDictVal>::calculate_serialized_size (opstr);
    }
  }
};

struct FontDictValuesMod
{
  inline void init (const CFF1FontDictValues *base_,
                    unsigned int fontName_,
                    const TableInfo &privateDictInfo_)
  {
    base = base_;
    fontName = fontName_;
    privateDictInfo = privateDictInfo_;
  }

  inline unsigned getNumValues (void) const
  {
    return base->getNumValues ();
  }

  inline const OpStr &operator [] (unsigned int i) const { return (*base)[i]; }

  const CFF1FontDictValues    *base;
  TableInfo                   privateDictInfo;
  unsigned int                fontName;
};

struct CFF1FontDict_OpSerializer : CFFFontDict_OpSerializer
{
  inline bool serialize (hb_serialize_context_t *c,
                         const OpStr &opstr,
                         const FontDictValuesMod &mod) const
  {
    TRACE_SERIALIZE (this);

    if (opstr.op == OpCode_FontName)
      return_trace (FontDict::serialize_uint2_op (c, opstr.op, mod.fontName));
    else
      return_trace (SUPER::serialize (c, opstr, mod.privateDictInfo));
  }

  inline unsigned int calculate_serialized_size (const OpStr &opstr) const
  {
    if (opstr.op == OpCode_FontName)
      return OpCode_Size (OpCode_shortint) + 2 + OpCode_Size (OpCode_FontName);
    else
      return SUPER::calculate_serialized_size (opstr);
  }

  private:
  typedef CFFFontDict_OpSerializer SUPER;
};

struct CFF1CSOpSet_Flatten : CFF1CSOpSet<CFF1CSOpSet_Flatten, FlattenParam>
{
  static inline void flush_args_and_op (OpCode op, CFF1CSInterpEnv &env, FlattenParam& param)
  {
    switch (op)
    {
      case OpCode_hstem:
      case OpCode_hstemhm:
      case OpCode_vstem:
      case OpCode_vstemhm:
      case OpCode_hintmask:
      case OpCode_cntrmask:
      case OpCode_hflex:
      case OpCode_flex:
      case OpCode_hflex1:
      case OpCode_flex1:
        if (param.drop_hints)
        {
          env.clear_args ();
          return;
        }
        /* NO BREAK */

      default:
        SUPER::flush_args_and_op (op, env, param);
        break;
    }
  }

  static inline void flush_n_args (unsigned int n, CFF1CSInterpEnv &env, FlattenParam& param)
  {
    for (unsigned int i = env.argStack.count - n; i < env.argStack.count; i++)
      param.flatStr.encode_num (env.argStack.elements[i]);
    SUPER::flush_n_args (n, env, param);
  }

  static inline void flush_op (OpCode op, CFF1CSInterpEnv &env, FlattenParam& param)
  {
    param.flatStr.encode_op (op);
  }

  static inline void flush_hintmask (OpCode op, CFF1CSInterpEnv &env, FlattenParam& param)
  {
    SUPER::flush_hintmask (op, env, param);
    if (!param.drop_hints)
      for (unsigned int i = 0; i < env.hintmask_size; i++)
        param.flatStr.encode_byte (env.substr[i]);
  }

  private:
  typedef CFF1CSOpSet<CFF1CSOpSet_Flatten, FlattenParam> SUPER;
};

struct RangeList : hb_vector_t<code_pair>
{
  /* replace the first glyph ID in the "glyph" field each range with a nLeft value */
  inline void finalize (unsigned int last_glyph)
  {
    for (unsigned int i = (*this).len; i > 0; i--)
    {
      code_pair &pair = (*this)[i - 1];
      unsigned int  nLeft = last_glyph - pair.glyph - 1;
      last_glyph = pair.glyph;
      pair.glyph = nLeft;
    }
  }
};

struct cff_subset_plan {
  inline cff_subset_plan (void)
    : final_size (0),
      orig_fdcount (0),
      subset_fdcount (1),
      subset_fdselect_format (0),
      offsets (),
      drop_hints (false)
  {
    topdict_sizes.init ();
    topdict_sizes.resize (1);
    topdict_mod.init ();
    subset_fdselect_ranges.init ();
    fdmap.init ();
    subset_charstrings.init ();
    flat_charstrings.init ();
    fontdicts_mod.init ();
    subset_enc_code_ranges.init ();
    subset_enc_supp_codes.init ();
    subset_charset_ranges.init ();
    sidmap.init ();
    for (unsigned int i = 0; i < NameDictValues::ValCount; i++)
      topDictModSIDs[i] = CFF_UNDEF_SID;
  }

  inline ~cff_subset_plan (void)
  {
    topdict_sizes.fini ();
    topdict_mod.fini ();
    subset_fdselect_ranges.fini ();
    fdmap.fini ();
    subset_charstrings.fini ();
    flat_charstrings.fini ();
    fontdicts_mod.fini ();
    subset_enc_code_ranges.fini ();
    subset_enc_supp_codes.init ();
    subset_charset_ranges.fini ();
    sidmap.fini ();
    fontdicts_mod.fini ();
  }

  inline unsigned int plan_subset_encoding (const OT::cff1::accelerator_subset_t &acc, hb_subset_plan_t *plan)
  {
    const Encoding *encoding = acc.encoding;
    unsigned int  size0, size1, supp_size;
    hb_codepoint_t  code, last_code = CFF_UNDEF_CODE;
    hb_vector_t<hb_codepoint_t> supp_codes;

    subset_enc_code_ranges.resize (0);
    supp_size = 0;
    supp_codes.init ();

    subset_enc_num_codes = plan->glyphs.len - 1;
    unsigned int glyph;
    for (glyph = 1; glyph < plan->glyphs.len; glyph++)
    {
      hb_codepoint_t  orig_glyph = plan->glyphs[glyph];
      code = acc.glyph_to_code (orig_glyph);
      if (code == CFF_UNDEF_CODE)
      {
        subset_enc_num_codes = glyph - 1;
        break;
      }

      if (code != last_code + 1)
      {
        code_pair pair = { code, glyph };
        subset_enc_code_ranges.push (pair);
      }
      last_code = code;

      if (encoding != &Null(Encoding))
      {
        hb_codepoint_t  sid = acc.glyph_to_sid (orig_glyph);
        encoding->get_supplement_codes (sid, supp_codes);
        for (unsigned int i = 0; i < supp_codes.len; i++)
        {
          code_pair pair = { supp_codes[i], sid };
          subset_enc_supp_codes.push (pair);
        }
        supp_size += SuppEncoding::static_size * supp_codes.len;
      }
    }
    supp_codes.fini ();

    subset_enc_code_ranges.finalize (glyph);

    assert (subset_enc_num_codes <= 0xFF);
    size0 = Encoding0::min_size + HBUINT8::static_size * subset_enc_num_codes;
    size1 = Encoding1::min_size + Encoding1_Range::static_size * subset_enc_code_ranges.len;

    if (size0 < size1)
      subset_enc_format = 0;
    else
      subset_enc_format = 1;

    return Encoding::calculate_serialized_size (
                        subset_enc_format,
                        subset_enc_format? subset_enc_code_ranges.len: subset_enc_num_codes,
                        subset_enc_supp_codes.len);
  }

  inline unsigned int plan_subset_charset (const OT::cff1::accelerator_subset_t &acc, hb_subset_plan_t *plan)
  {
    unsigned int  size0, size_ranges;
    hb_codepoint_t  sid, last_sid = CFF_UNDEF_CODE;
    bool  two_byte = false;

    subset_charset_ranges.resize (0);
    unsigned int glyph;
    for (glyph = 1; glyph < plan->glyphs.len; glyph++)
    {
      hb_codepoint_t  orig_glyph = plan->glyphs[glyph];
      sid = acc.glyph_to_sid (orig_glyph);

      if (!acc.is_CID ())
        sid = sidmap.add (sid);

      if (sid != last_sid + 1)
      {
        code_pair pair = { sid, glyph };
        subset_charset_ranges.push (pair);
      }
      last_sid = sid;
    }

    subset_charset_ranges.finalize (glyph);

    size0 = Charset0::min_size + HBUINT16::static_size * (plan->glyphs.len - 1);
    if (!two_byte)
      size_ranges = Charset1::min_size + Charset1_Range::static_size * subset_charset_ranges.len;
    else
      size_ranges = Charset2::min_size + Charset2_Range::static_size * subset_charset_ranges.len;

    if (size0 < size_ranges)
      subset_charset_format = 0;
    else if (!two_byte)
      subset_charset_format = 1;
    else
      subset_charset_format = 2;

    return Charset::calculate_serialized_size (
                        subset_charset_format,
                        subset_charset_format? subset_charset_ranges.len: plan->glyphs.len);
  }

  inline bool collect_sids_in_dicts (const OT::cff1::accelerator_subset_t &acc)
  {
    if (unlikely (!sidmap.reset (acc.stringIndex->count)))
      return false;
    
    for (unsigned int i = 0; i < NameDictValues::ValCount; i++)
    {
      unsigned int sid = acc.topDict.nameSIDs[i];
      if (sid != CFF_UNDEF_SID)
      {
        (void)sidmap.add (sid);
        topDictModSIDs[i] = sidmap[sid];
      }
    }

    if (acc.fdArray != &Null(CFF1FDArray))
      for (unsigned int fd = 0; fd < orig_fdcount; fd++)
        if (!fdmap.excludes (fd))
          (void)sidmap.add (acc.fontDicts[fd].fontName);
  
    return true;
  }

  inline bool create (const OT::cff1::accelerator_subset_t &acc,
                      hb_subset_plan_t *plan)
  {
     /* make sure notdef is first */
    if ((plan->glyphs.len == 0) || (plan->glyphs[0] != 0)) return false;

    final_size = 0;
    num_glyphs = plan->glyphs.len;
    orig_fdcount = acc.fdCount;
    drop_hints = plan->drop_hints;

    /* check whether the subset renumbers any glyph IDs */
    gid_renum = false;
    for (unsigned int glyph = 0; glyph < plan->glyphs.len; glyph++)
    {
      if (plan->glyphs[glyph] != glyph) {
        gid_renum = true;
        break;
      }
    }

    subset_charset = gid_renum || !acc.is_predef_charset ();
    subset_encoding = !acc.is_CID() && (gid_renum || !acc.is_predef_encoding ());

    /* CFF header */
    final_size += OT::cff1::static_size;
    
    /* Name INDEX */
    offsets.nameIndexOffset = final_size;
    final_size += acc.nameIndex->get_size ();

    /* top dict INDEX */
    {
      /* Add encoding/charset to a (copy of) top dict as necessary */
      topdict_mod.init (&acc.topDict);
      bool need_to_add_enc = (subset_encoding && !acc.topDict.hasOp (OpCode_Encoding));
      bool need_to_add_set = (subset_charset && !acc.topDict.hasOp (OpCode_charset));
      if (need_to_add_enc || need_to_add_set)
      {
        if (need_to_add_enc)
          topdict_mod.addOp (OpCode_Encoding);
        if (need_to_add_set)
          topdict_mod.addOp (OpCode_charset);
      }
      offsets.topDictInfo.offset = final_size;
      CFF1TopDict_OpSerializer topSzr;
      unsigned int topDictSize = TopDict::calculate_serialized_size (topdict_mod, topSzr);
      offsets.topDictInfo.offSize = calcOffSize(topDictSize);
      final_size += CFF1IndexOf<TopDict>::calculate_serialized_size<CFF1TopDictValuesMod>
                                                (offsets.topDictInfo.offSize,
                                                 &topdict_mod, 1, topdict_sizes, topSzr);
    }

    /* Determine re-mapping of font index as fdmap among other info */
    if (acc.fdSelect != &Null(CFF1FDSelect)
        && unlikely (!hb_plan_subset_cff_fdselect (plan->glyphs,
                                  orig_fdcount,
                                  *acc.fdSelect,
                                  subset_fdcount,
                                  offsets.FDSelectInfo.size,
                                  subset_fdselect_format,
                                  subset_fdselect_ranges,
                                  fdmap)))
        return false;

    /* remove unused SIDs & reassign SIDs */
    {
      /* SIDs for name strings in dicts are added before glyph names so they fit in 16-bit int range */
      if (unlikely (!collect_sids_in_dicts (acc)))
        return false;
      assert (sidmap.get_count () <= 0x8000);
      if (subset_charset)
        offsets.charsetInfo.size = plan_subset_charset (acc, plan);

      topdict_mod.reassignSIDs (sidmap);
    }

    /* String INDEX */
    {
      offsets.stringIndexInfo.offset = final_size;
      offsets.stringIndexInfo.size = acc.stringIndex->calculate_serialized_size (offsets.stringIndexInfo.offSize, sidmap);
      final_size += offsets.stringIndexInfo.size;
    }
    
    {
      /* Flatten global & local subrs */
      SubrFlattener<const OT::cff1::accelerator_subset_t, CFF1CSInterpEnv, CFF1CSOpSet_Flatten>
                    flattener(acc, plan->glyphs, plan->drop_hints);
      if (!flattener.flatten (flat_charstrings))
        return false;
      
      /* no global/local subroutines */
      offsets.globalSubrsInfo.size = HBUINT16::static_size; /* count 0 only */
    }

    /* global subrs */
    offsets.globalSubrsInfo.offset = final_size;
    final_size += offsets.globalSubrsInfo.size;

    /* Encoding */
    offsets.encodingOffset = final_size;
    if (subset_encoding)
      final_size += plan_subset_encoding (acc, plan);

    /* Charset */
    offsets.charsetInfo.offset = final_size;
    final_size += offsets.charsetInfo.size;

    /* FDSelect */
    if (acc.fdSelect != &Null(CFF1FDSelect))
    {
      offsets.FDSelectInfo.offset = final_size;
      if (!is_fds_subsetted ())
        offsets.FDSelectInfo.size = acc.fdSelect->calculate_serialized_size (acc.num_glyphs);
      final_size += offsets.FDSelectInfo.size;
    }

    /* FDArray (FDIndex) */
    if (acc.fdArray != &Null(CFF1FDArray)) {
      offsets.FDArrayInfo.offset = final_size;
      CFF1FontDict_OpSerializer fontSzr;
      unsigned int dictsSize = 0;
      for (unsigned int i = 0; i < acc.fontDicts.len; i++)
        if (!fdmap.excludes (i))
          dictsSize += FontDict::calculate_serialized_size (acc.fontDicts[i], fontSzr);

      offsets.FDArrayInfo.offSize = calcOffSize (dictsSize);
      final_size += CFF1Index::calculate_serialized_size (offsets.FDArrayInfo.offSize, subset_fdcount, dictsSize);
    }

    /* CharStrings */
    {
      offsets.charStringsInfo.offset = final_size;
      unsigned int dataSize = 0;
      for (unsigned int i = 0; i < plan->glyphs.len; i++)
      {
        ByteStrBuff &flatstr = flat_charstrings[i];
        ByteStr str (&flatstr[0], flatstr.len);
        subset_charstrings.push (str);
        dataSize += flatstr.len;
      }
      offsets.charStringsInfo.offSize = calcOffSize (dataSize);
      final_size += CFF1CharStrings::calculate_serialized_size (offsets.charStringsInfo.offSize, plan->glyphs.len, dataSize);
    }

    /* private dicts & local subrs */
    offsets.privateDictInfo.offset = final_size;
    for (unsigned int i = 0; i < orig_fdcount; i++)
    {
      if (!fdmap.excludes (i))
      {
        CFFPrivateDict_OpSerializer privSzr (plan->drop_hints);
        unsigned int  priv_size = PrivateDict::calculate_serialized_size (acc.privateDicts[i], privSzr);
        TableInfo  privInfo = { final_size, priv_size, 0 };
        FontDictValuesMod fontdict_mod;
        fontdict_mod.init ( &acc.fontDicts[i], sidmap[acc.fontDicts[i].fontName], privInfo );
        fontdicts_mod.push (fontdict_mod);
        final_size += privInfo.size;
      }
    }

    if (!acc.is_CID ())
      offsets.privateDictInfo = fontdicts_mod[0].privateDictInfo;

    return ((subset_charstrings.len == plan->glyphs.len) &&
            (fontdicts_mod.len == subset_fdcount));
  }

  inline unsigned int get_final_size (void) const  { return final_size; }

  unsigned int              final_size;
  hb_vector_t<unsigned int> topdict_sizes;
  CFF1TopDictValuesMod      topdict_mod;
  CFF1SubTableOffsets       offsets;

  unsigned int    num_glyphs;
  unsigned int    orig_fdcount;
  unsigned int    subset_fdcount;
  inline bool     is_fds_subsetted (void) const { return subset_fdcount < orig_fdcount; }
  unsigned int    subset_fdselect_format;
  hb_vector_t<code_pair>   subset_fdselect_ranges;

  /* font dict index remap table from fullset FDArray to subset FDArray.
   * set to CFF_UNDEF_CODE if excluded from subset */
  Remap   fdmap;

  hb_vector_t<ByteStr>    subset_charstrings;
  ByteStrBuffArray        flat_charstrings;
  hb_vector_t<FontDictValuesMod>  fontdicts_mod;

  bool                    flatten_subrs;
  bool                    drop_hints;

  bool                    gid_renum;
  bool                    subset_encoding;
  uint8_t                 subset_enc_format;
  unsigned int            subset_enc_num_codes;
  RangeList               subset_enc_code_ranges;
  hb_vector_t<code_pair>  subset_enc_supp_codes;

  uint8_t                 subset_charset_format;
  RangeList               subset_charset_ranges;
  bool                    subset_charset;

  RemapSID                sidmap;
  unsigned int            topDictModSIDs[NameDictValues::ValCount];
};

static inline bool _write_cff1 (const cff_subset_plan &plan,
                                const OT::cff1::accelerator_subset_t  &acc,
                                const hb_vector_t<hb_codepoint_t>& glyphs,
                                unsigned int dest_sz,
                                void *dest)
{
  hb_serialize_context_t c (dest, dest_sz);

  char RETURN_OP[1] = { OpCode_return };
  const ByteStr NULL_SUBR (RETURN_OP, 1);

  OT::cff1 *cff = c.start_serialize<OT::cff1> ();
  if (unlikely (!c.extend_min (*cff)))
    return false;

  /* header */
  cff->version.major.set (0x01);
  cff->version.minor.set (0x00);
  cff->nameIndex.set (cff->min_size);
  cff->offSize.set (4); /* unused? */

  /* name INDEX */
  {
    assert (cff->nameIndex == c.head - c.start);
    CFF1NameIndex *dest = c.start_embed<CFF1NameIndex> ();
    if (unlikely (dest == nullptr)) return false;
    if (unlikely (!dest->serialize (&c, *acc.nameIndex)))
    {
      DEBUG_MSG (SUBSET, nullptr, "failed to serialize CFF name INDEX");
      return false;
    }
  }

  /* top dict INDEX */
  {
    assert (plan.offsets.topDictInfo.offset == c.head - c.start);
    CFF1IndexOf<TopDict> *dest = c.start_embed< CFF1IndexOf<TopDict> > ();
    if (dest == nullptr) return false;
    CFF1TopDict_OpSerializer topSzr;
    TopDictModifiers  modifier (plan.offsets, plan.topDictModSIDs);
    if (unlikely (!dest->serialize (&c, plan.offsets.topDictInfo.offSize,
                                    &plan.topdict_mod, 1,
                                    plan.topdict_sizes, topSzr, modifier)))
    {
      DEBUG_MSG (SUBSET, nullptr, "failed to serialize CFF top dict");
      return false;
    }
  }

  /* String INDEX */
  {
    assert (plan.offsets.stringIndexInfo.offset == c.head - c.start);
    CFF1StringIndex *dest = c.start_embed<CFF1StringIndex> ();
    if (unlikely (dest == nullptr)) return false;
    if (unlikely (!dest->serialize (&c, *acc.stringIndex, plan.offsets.stringIndexInfo.offSize, plan.sidmap)))
    {
      DEBUG_MSG (SUBSET, nullptr, "failed to serialize CFF string INDEX");
      return false;
    }
  }

  /* global subrs */
  {
    assert (plan.offsets.globalSubrsInfo.offset != 0);
    assert (plan.offsets.globalSubrsInfo.offset == c.head - c.start);
    CFF1Subrs *dest = c.allocate_size <CFF1Subrs> (HBUINT16::static_size);
    if (unlikely (dest == nullptr)) return false;
    dest->count.set (0);
  }

  /* Encoding */
  if (plan.subset_encoding)
  {
    assert (plan.offsets.encodingOffset == c.head - c.start);
    Encoding *dest = c.start_embed<Encoding> ();
    if (unlikely (dest == nullptr)) return false;
    if (unlikely (!dest->serialize (&c,
                                    plan.subset_enc_format,
                                    plan.subset_enc_num_codes,
                                    plan.subset_enc_code_ranges,
                                    plan.subset_enc_supp_codes)))
    {
      DEBUG_MSG (SUBSET, nullptr, "failed to serialize Encoding");
      return false;
    }
  }

  /* Charset */
  if (plan.subset_charset)
  {
    assert (plan.offsets.charsetInfo.offset == c.head - c.start);
    Charset *dest = c.start_embed<Charset> ();
    if (unlikely (dest == nullptr)) return false;
    if (unlikely (!dest->serialize (&c,
                                    plan.subset_charset_format,
                                    plan.num_glyphs,
                                    plan.subset_charset_ranges)))
    {
      DEBUG_MSG (SUBSET, nullptr, "failed to serialize Charset");
      return false;
    }
  }

  /* FDSelect */
  if (acc.fdSelect != &Null(CFF1FDSelect))
  {
    assert (plan.offsets.FDSelectInfo.offset == c.head - c.start);
    
    if (plan.is_fds_subsetted ())
    {
      if (unlikely (!hb_serialize_cff_fdselect (&c, glyphs.len, *acc.fdSelect, acc.fdCount,
                                                plan.subset_fdselect_format, plan.offsets.FDSelectInfo.size,
                                                plan.subset_fdselect_ranges,
                                                plan.fdmap)))
      {
        DEBUG_MSG (SUBSET, nullptr, "failed to serialize CFF subset FDSelect");
        return false;
      }
    }
    else
    {
      CFF1FDSelect *dest = c.start_embed<CFF1FDSelect> ();
      if (unlikely (!dest->serialize (&c, *acc.fdSelect, acc.num_glyphs)))
      {
        DEBUG_MSG (SUBSET, nullptr, "failed to serialize CFF FDSelect");
        return false;
      }
    }
  }

  /* FDArray (FD Index) */
  if (acc.fdArray != &Null(CFF1FDArray))
  {
    assert (plan.offsets.FDArrayInfo.offset == c.head - c.start);
    CFF1FDArray  *fda = c.start_embed<CFF1FDArray> ();
    if (unlikely (fda == nullptr)) return false;
    CFF1FontDict_OpSerializer  fontSzr;
    if (unlikely (!fda->serialize (&c, plan.offsets.FDArrayInfo.offSize,
                                   plan.fontdicts_mod,
                                   fontSzr)))
    {
      DEBUG_MSG (SUBSET, nullptr, "failed to serialize CFF FDArray");
      return false;
    }
  }

  /* CharStrings */
  {
    assert (plan.offsets.charStringsInfo.offset == c.head - c.start);
    CFF1CharStrings  *cs = c.start_embed<CFF1CharStrings> ();
    if (unlikely (cs == nullptr)) return false;
    if (unlikely (!cs->serialize (&c, plan.offsets.charStringsInfo.offSize, plan.subset_charstrings)))
    {
      DEBUG_MSG (SUBSET, nullptr, "failed to serialize CFF CharStrings");
      return false;
    }
  }

  /* private dicts & local subrs */
  assert (plan.offsets.privateDictInfo.offset == c.head - c.start);
  for (unsigned int i = 0; i < acc.privateDicts.len; i++)
  {
    if (!plan.fdmap.excludes (i))
    {
      PrivateDict  *pd = c.start_embed<PrivateDict> ();
      if (unlikely (pd == nullptr)) return false;
      unsigned int priv_size = plan.fontdicts_mod[plan.fdmap[i]].privateDictInfo.size;
      bool result;
      CFFPrivateDict_OpSerializer privSzr (plan.drop_hints);
      /* N.B. local subrs immediately follows its corresponding private dict. i.e., subr offset == private dict size */
      result = pd->serialize (&c, acc.privateDicts[i], privSzr, priv_size);
      if (unlikely (!result))
      {
        DEBUG_MSG (SUBSET, nullptr, "failed to serialize CFF Private Dict[%d]", i);
        return false;
      }
    }
  }

  assert (c.head == c.end);
  c.end_serialize ();

  return true;
}

static bool
_hb_subset_cff1 (const OT::cff1::accelerator_subset_t  &acc,
                const char                      *data,
                hb_subset_plan_t                *plan,
                hb_blob_t                       **prime /* OUT */)
{
  cff_subset_plan cff_plan;

  if (unlikely (!cff_plan.create (acc, plan)))
  {
    DEBUG_MSG(SUBSET, nullptr, "Failed to generate a cff subsetting plan.");
    return false;
  }

  unsigned int  cff_prime_size = cff_plan.get_final_size ();
  char *cff_prime_data = (char *) calloc (1, cff_prime_size);

  if (unlikely (!_write_cff1 (cff_plan, acc, plan->glyphs,
                              cff_prime_size, cff_prime_data))) {
    DEBUG_MSG(SUBSET, nullptr, "Failed to write a subset cff.");
    free (cff_prime_data);
    return false;
  }

  *prime = hb_blob_create (cff_prime_data,
                           cff_prime_size,
                           HB_MEMORY_MODE_READONLY,
                           cff_prime_data,
                           free);
  return true;
}

/**
 * hb_subset_cff1:
 * Subsets the CFF table according to a provided plan.
 *
 * Return value: subsetted cff table.
 **/
bool
hb_subset_cff1 (hb_subset_plan_t *plan,
                hb_blob_t       **prime /* OUT */)
{
  hb_blob_t *cff_blob = hb_sanitize_context_t().reference_table<CFF::cff1> (plan->source);
  const char *data = hb_blob_get_data(cff_blob, nullptr);

  OT::cff1::accelerator_subset_t acc;
  acc.init(plan->source);
  bool result = likely (acc.is_valid ()) &&
                        _hb_subset_cff1 (acc, data, plan, prime);
  hb_blob_destroy (cff_blob);
  acc.fini ();

  return result;
}
