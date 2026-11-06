// Translates Xenos vertex shaders (decoded, xenos_ucode.h) into NV2A vertex
// programs, so vertex work runs on the GPU instead of the CPU interpreter.
// Anything not handled (predicates, rare ops, too many temporaries or
// instructions) returns nullptr and the draw uses the interpreter instead.
//
// NV2A instruction encoding: pbkit nv20_shader.h (checked against nxdk's
// vp20compiler output). Notes that matter here:
//  - one constant and one input register per instruction;
//  - ADD reads operands A and C; ILU ops (RCP, RSQ, EXP...) read C;
//  - no abs, saturate, frc, sin or cos: built from MAX/MIN, EXP's .y
//    (fraction) and a parabola approximation.
//
// Register use: Xenos r0-r3 -> R0-R3; R4 vector staging; R5-R7 per-source
// scratch; R8, R9 scratch (frc, sin/cos, epilogue); R10 position; R11 the
// previous scalar result.
// Constant slots: 0 = (0, 1, 0, 0), 1 = (1/2pi, 0.5, 2pi, pi),
// 2 = (4/pi, -4/pi^2, 0.225, pi/2), 3 = viewport (scale x, y, offset x, y),
// 4+ = the shader's own constants.
#include "xenos_vs.h"

#include <initializer_list>

#include "xenos_ucode.h"

namespace {

using xenos::Instruction;
using xenos::Program;
using xenos::Source;

enum : u8 { kNone = 0, kTemp = 1, kInput = 2, kConst = 3 };
enum Mac : u8 { MOV = 1, MUL = 2, ADD = 3, MAD = 4, DP3 = 5, DP4 = 7, MIN = 9, MAX = 10, SLT = 11, SGE = 12 };
enum Ilu : u8 { IMOV = 1, RCP = 2, RCC = 3, RSQ = 4, EXP = 5, LOG = 6 };

constexpr u8 kStage = 4, kPos = 10, kPrev = 11, kS0 = 8, kS1 = 9;
constexpr u8 kOutPos = 0, kOutD0 = 3, kOutT0 = 9;
constexpr int kMaxNv2a = 120;  // slots 8..127; 0..7 pass-through, 128..135 the glow passes

struct Op {
  u8 type = kNone;
  u8 index = 0;
  u8 swz[4] = {0, 1, 2, 3};
  bool neg = false;
};

Op temp(u8 r, const char* sw = "xyzw") {
  Op o;
  o.type = kTemp;
  o.index = r;
  for (int i = 0; i < 4; ++i) o.swz[i] = (u8)(sw[i] == 'x' ? 0 : sw[i] == 'y' ? 1 : sw[i] == 'z' ? 2 : 3);
  return o;
}

Op cnst(u8 slot, const char* sw = "xyzw") {
  Op o = temp(slot, sw);
  o.type = kConst;
  return o;
}

Op neg(Op o) {
  o.neg = !o.neg;
  return o;
}

Op replicate(Op o, u8 component) {
  u8 c = o.swz[component];
  for (u8& s : o.swz) s = c;
  return o;
}

struct Builder {
  XvpProgram& out;
  bool failed = false;
  int count = 0;
  u32 words[kMaxNv2a * 4];

  // Destination: temp (mask) and/or output (mask).
  struct Dest {
    int temp = -1;
    u8 temp_mask = 0;  // xenos order: bit0 = x
    int output = -1;
    u8 out_mask = 0;
  };

  static u32 nv_mask(u8 m) { return ((m & 1) << 3) | ((m & 2) << 1) | ((m & 4) >> 1) | ((m & 8) >> 3); }

  u32 encode_src(const Op& o) {
    if (o.type == kNone) return 0x06C0 | kTemp;  // unused: identity swizzle
    u32 v = (u32)o.type | ((u32)(o.type == kTemp ? o.index : 0) << 2);
    v |= (u32)o.swz[0] << 12 | (u32)o.swz[1] << 10 | (u32)o.swz[2] << 8 | (u32)o.swz[3] << 6;
    if (o.neg) v |= 1u << 14;
    return v;
  }

  // a, b, c are the MAC operands (ILU reads c).
  void emit(u8 mac, u8 ilu, Op a, Op b, Op c, const Dest& d) {
    if (failed) return;
    int const_slot = -1, input = -1;
    for (const Op* o : {&a, &b, &c}) {
      if (o->type == kConst) {
        if (const_slot >= 0 && const_slot != o->index) return fail();
        const_slot = o->index;
      } else if (o->type == kInput) {
        if (input >= 0 && input != o->index) return fail();
        input = o->index;
      }
    }
    if (count >= kMaxNv2a) return fail();
    u32 sa = encode_src(a), sb = encode_src(b), sc = encode_src(c);
    u32* w = &words[count * 4];
    w[0] = 0;
    w[1] = (u32)ilu << 25 | (u32)mac << 21 | (u32)((const_slot >= 0 ? const_slot : 0) + 96) << 13 |
           (u32)(input >= 0 ? input : 0) << 9 | (sa >> 6);
    w[2] = (sa & 0x3F) << 26 | sb << 11 | (sc >> 4);
    u32 d3 = (sc & 0xF) << 28;
    u32 mask = d.temp >= 0 ? nv_mask(d.temp_mask) : 0;
    if (mac) d3 |= mask << 24;
    else d3 |= mask << 16;
    d3 |= (u32)(d.temp >= 0 ? d.temp : 7) << 20;
    if (d.output >= 0) {
      d3 |= nv_mask(d.out_mask) << 12 | 1u << 11 | (u32)d.output << 3;
      if (!mac) d3 |= 1u << 2;  // output written by the ILU
    }
    w[3] = d3;
    ++count;
  }

  void fail() { failed = true; }

  Dest to_temp(u8 r, u8 mask = 0xF) {
    Dest d;
    d.temp = r;
    d.temp_mask = mask;
    return d;
  }

  void mac(u8 op, Op a, Op b, Op c, Dest d) { emit(op, 0, a, b, c, d); }
  void mov(Dest d, Op a) { mac(MOV, a, Op(), Op(), d); }
  void ilu(u8 op, Op c, Dest d) { emit(0, op, Op(), Op(), c, d); }

  // frac(x.component) into tmp.y via EXP (its .y is x - floor(x)).
  void frac_into(Op x, u8 tmp) { ilu(EXP, x, to_temp(tmp, 2)); }
};

// Xenos source -> operand, resolving abs into a scratch temp.
Op operand(Builder& b, const Source& s, int which) {
  Op o;
  if (s.temp) {
    if (s.index > 3) b.fail();
    o.type = kTemp;
    o.index = s.index;
  } else {
    int slot = -1;
    for (u32 i = 0; i < b.out.const_count; ++i) {
      if (b.out.const_xenos[i] == s.index) slot = (int)i;
    }
    if (slot < 0) {
      if (b.out.const_count >= 60) {
        b.fail();
        return o;
      }
      slot = (int)b.out.const_count;
      b.out.const_xenos[b.out.const_count++] = s.index;
    }
    o.type = kConst;
    o.index = (u8)(slot + 4);
  }
  if (s.absolute) {
    u8 scratch = (u8)(5 + which);
    b.mac(MAX, o, neg(o), Op(), b.to_temp(scratch));
    o = Op();
    o.type = kTemp;
    o.index = scratch;
  }
  for (int c = 0; c < 4; ++c) o.swz[c] = s.swizzle[c];
  o.neg = s.negate;
  return o;
}

// Moves the second constant of an instruction into scratch when two
// different constants are read.
void split_constants(Builder& b, Op* ops, int n) {
  int first = -1;
  for (int i = 0; i < n; ++i) {
    if (ops[i].type != kConst) continue;
    if (first < 0) {
      first = ops[i].index;
    } else if (ops[i].index != first) {
      u8 scratch = (u8)(5 + i);
      Op whole = cnst(ops[i].index);
      b.mov(b.to_temp(scratch), whole);
      ops[i].type = kTemp;
      ops[i].index = scratch;
    }
  }
}

// sin or cos of operand x (replicated) into dest temp, all components.
void sincos(Builder& b, Op x, bool cosine, u8 dest) {
  if (cosine) {
    b.mac(ADD, x, Op(), cnst(2, "wwww"), b.to_temp(kS1));
    x = temp(kS1, "xxxx");
  }
  // t = x / 2pi + 0.5; y = frac(t) * 2pi - pi, in [-pi, pi)
  b.mac(MAD, x, cnst(1, "xxxx"), cnst(1, "yyyy"), b.to_temp(kS1));
  b.frac_into(temp(kS1, "xxxx"), kS0);
  b.mac(MAD, temp(kS0, "yyyy"), cnst(1, "zzzz"), neg(cnst(1, "wwww")), b.to_temp(kS1));
  // s = 4/pi y - 4/pi^2 y|y|
  b.mac(MAX, temp(kS1), neg(temp(kS1)), Op(), b.to_temp(kS0));
  b.mac(MUL, temp(kS1), temp(kS0), Op(), b.to_temp(kS0));
  b.mac(MUL, temp(kS0), cnst(2, "yyyy"), Op(), b.to_temp(kS0));
  b.mac(MAD, temp(kS1), cnst(2, "xxxx"), temp(kS0), b.to_temp(kS0));
  // s += 0.225 (s|s| - s)
  b.mac(MAX, temp(kS0), neg(temp(kS0)), Op(), b.to_temp(kS1));
  b.mac(MAD, temp(kS0), temp(kS1), neg(temp(kS0)), b.to_temp(kS1));
  b.mac(MAD, temp(kS1), cnst(2, "zzzz"), temp(kS0), b.to_temp(dest));
}

struct Variant {
  XvpProgram program;
  bool used;
};
Variant g_variants[48];

}  // namespace

const XvpProgram* xvp_get(const XvsProgram* xvs, int uv_i, int color_i, bool divide) {
  const Program& p = *(const Program*)xvs;
  u32 key = p.hash ^ (u32)(uv_i + 2) << 4 ^ (u32)(color_i + 2) << 8 ^ (u32)divide << 12;
  for (Variant& v : g_variants) {
    if (v.used && v.program.key == key) return v.program.ok ? &v.program : nullptr;
  }
  Variant* slot = nullptr;
  for (Variant& v : g_variants) {
    if (!v.used) {
      slot = &v;
      break;
    }
  }
  if (!slot) return nullptr;
  slot->used = true;
  XvpProgram& out = slot->program;
  out = {};
  out.key = key;
  Builder b{out};

  // Defaults: colour (0,0,0,1), texcoord (0,0,0,1).
  Builder::Dest d0;
  d0.output = kOutD0;
  d0.out_mask = 0xF;
  b.mov(d0, cnst(0, "xxxy"));
  Builder::Dest t0;
  t0.output = kOutT0;
  t0.out_mask = 0xF;
  b.mov(t0, cnst(0, "xxxy"));

  // Where an exported register lands.
  auto export_dest = [&](u8 dst, u8 mask) {
    Builder::Dest d;
    if (dst == 62) {
      d.temp = kPos;
      d.temp_mask = mask;
    } else if (dst == uv_i) {
      d.output = kOutT0;
      d.out_mask = mask;
    } else if (dst == color_i) {
      d.output = kOutD0;
      d.out_mask = mask;
    }
    return d;
  };

  for (int n = 0; n < p.count && !b.failed; ++n) {
    const Instruction& in = p.ins[n];
    if (in.fetch) {
      if (in.format == 0xFF) continue;
      if (in.dst > 3) return b.fail(), nullptr;
      if (!in.mini) {
        if (out.stride && (out.fetch_slot != in.slot || out.stride != in.stride)) return b.fail(), nullptr;
        out.fetch_slot = in.slot;
        out.stride = in.stride;
      }
      u8 components = in.format == 36 ? 1 : in.format == 37 ? 2 : in.format == 57 ? 3 : in.format == 38 ? 4
                    : in.format == 6 ? 4 : 0;
      // 8_8_8_8 only as normalised unsigned bytes (NV2A UB_OGL).
      if (!components || (in.format == 6 && (!in.normalized || in.is_signed))) return b.fail(), nullptr;
      if (out.attr_count >= 15) return b.fail(), nullptr;
      int attr = -1;
      for (u32 a = 0; a < out.attr_count; ++a) {
        if (out.attrs[a].offset == in.offset && out.attrs[a].format == in.format) attr = (int)a;
      }
      if (attr < 0) {
        attr = (int)out.attr_count++;
        out.attrs[attr] = {in.offset, in.format, components};
      }
      Op v;
      v.type = kInput;
      v.index = (u8)attr;
      u8 from_attr = 0, zeros = 0, ones = 0;
      for (int c = 0; c < 4; ++c) {
        u8 s = in.dst_select[c];
        if (s == 7) continue;
        if (s < 4 && s < components) {
          from_attr |= 1 << c;
          v.swz[c] = s;
        } else if (s == 5 || (s == 3)) {
          ones |= 1 << c;
        } else {
          zeros |= 1 << c;
        }
      }
      if (from_attr) b.mov(b.to_temp(in.dst, from_attr), v);
      if (zeros) b.mov(b.to_temp(in.dst, zeros), cnst(0, "xxxx"));
      if (ones) b.mov(b.to_temp(in.dst, ones), cnst(0, "yyyy"));
      continue;
    }

    if (in.predicated) return b.fail(), nullptr;
    bool vector_used = in.vmask != 0;
    bool scalar_used = in.sop != 50 && (in.smask != 0 || true);
    if (in.sop == 50) scalar_used = false;
    bool scalar_writes = in.smask != 0 && in.sop != 50;

    Op src[3];
    for (int s = 0; s < 3; ++s) {
      bool needed = (s < 2 && vector_used) || (s == 2 && (vector_used || scalar_used));
      if (needed) src[s] = operand(b, in.s[s], s);
    }

    // Vector op into the stage register.
    if (vector_used) {
      Op a = src[0], bb = src[1], c = src[2];
      Op ops[3] = {a, bb, c};
      int nops = in.vop >= 11 && in.vop <= 14 ? 3 : 2;
      split_constants(b, ops, nops);
      a = ops[0], bb = ops[1], c = ops[2];
      Builder::Dest stage = b.to_temp(kStage);
      switch (in.vop) {
        case 0: b.mac(ADD, a, Op(), bb, stage); break;
        case 1: b.mac(MUL, a, bb, Op(), stage); break;
        case 2: b.mac(MAX, a, bb, Op(), stage); break;
        case 3: b.mac(MIN, a, bb, Op(), stage); break;
        case 5: b.mac(SLT, bb, a, Op(), stage); break;  // a > b == b < a
        case 6: b.mac(SGE, a, bb, Op(), stage); break;
        case 8:  // frc, per component
        case 10:  // floor = x - frc
          for (int k = 0; k < 4; ++k) {
            if (!(in.vmask & (1 << k))) continue;
            b.frac_into(replicate(a, (u8)k), kS0);
            Builder::Dest one = b.to_temp(kStage, (u8)(1 << k));
            if (in.vop == 8) b.mov(one, temp(kS0, "yyyy"));
            else b.mac(ADD, replicate(a, (u8)k), Op(), neg(temp(kS0, "yyyy")), one);
          }
          break;
        case 11: b.mac(MAD, a, bb, c, stage); break;
        case 12:  // cndeq: a == 0 ? b : c
        case 13:  // cndge: a >= 0 ? b : c
        case 14:  // cndgt: a > 0 ? b : c
          // mask m in kS0, then c + m * (b - c).
          if (a.type == kConst) {
            b.mov(b.to_temp(kS1), a);
            a = temp(kS1);
          }
          if (in.vop == 14) {
            b.mac(SLT, cnst(0, "xxxx"), a, Op(), b.to_temp(kS0));
          } else if (in.vop == 12) {
            // a == 0 as 0 >= |a|: testing -a >= 0 failed on the real NV2A,
            // which (unlike XEMU) seems to order -0 below 0; |a| of a zero
            // compares equal to 0 either way.
            b.mac(MAX, a, neg(a), Op(), b.to_temp(kS0));
            b.mac(SGE, cnst(0, "xxxx"), temp(kS0), Op(), b.to_temp(kS0));
          } else {
            b.mac(SGE, a, cnst(0, "xxxx"), Op(), b.to_temp(kS0));
          }
          b.mac(ADD, bb, Op(), neg(c), b.to_temp(kS1));
          b.mac(MAD, temp(kS0), temp(kS1), c, stage);
          break;
        case 15: b.mac(DP4, a, bb, Op(), stage); break;
        case 16: b.mac(DP3, a, bb, Op(), stage); break;
        default: return b.fail(), nullptr;
      }
      if (in.vclamp) {
        b.mac(MAX, temp(kStage), cnst(0, "xxxx"), Op(), stage);
        b.mac(MIN, temp(kStage), cnst(0, "yyyy"), Op(), stage);
      }
    }

    // Scalar op into the previous-scalar register (all components).
    if (scalar_used) {
      Op sa = replicate(src[2], 3), sb = replicate(src[2], 0);
      Op ops[2] = {sa, sb};
      split_constants(b, ops, 2);
      sa = ops[0], sb = ops[1];
      Builder::Dest prev = b.to_temp(kPrev);
      Op prev_op = temp(kPrev, "xxxx");
      switch (in.sop) {
        case 0: b.mac(ADD, sa, Op(), sb, prev); break;
        case 1: b.mac(ADD, sa, Op(), prev_op, prev); break;
        case 2: b.mac(MUL, sa, sb, Op(), prev); break;
        case 3: b.mac(MUL, sa, prev_op, Op(), prev); break;
        case 5: b.mac(MAX, sa, sb, Op(), prev); break;
        case 6: b.mac(MIN, sa, sb, Op(), prev); break;
        case 8: b.mac(SLT, cnst(0, "xxxx"), sa, Op(), prev); break;
        case 9: b.mac(SGE, sa, cnst(0, "xxxx"), Op(), prev); break;
        case 11: b.frac_into(sa, kS0); b.mov(prev, temp(kS0, "yyyy")); break;
        case 13: b.frac_into(sa, kS0); b.mac(ADD, sa, Op(), neg(temp(kS0, "yyyy")), prev); break;
        case 14: b.ilu(EXP, sa, b.to_temp(kS0, 4)); b.mov(prev, temp(kS0, "zzzz")); break;
        case 15: case 16: b.ilu(LOG, sa, b.to_temp(kS0, 4)); b.mov(prev, temp(kS0, "zzzz")); break;
        case 17: case 18: case 19: b.ilu(RCP, sa, prev); break;
        case 20: case 21: case 22: b.ilu(RSQ, sa, prev); break;
        case 25: b.mac(ADD, sa, Op(), neg(sb), prev); break;
        case 26: b.mac(ADD, sa, Op(), neg(prev_op), prev); break;
        case 40: b.ilu(RSQ, sa, b.to_temp(kS0)); b.mac(MUL, sa, temp(kS0), Op(), prev); break;
        case 48: sincos(b, sa, false, kPrev); break;
        case 49: sincos(b, sa, true, kPrev); break;
        default: return b.fail(), nullptr;
      }
      if (in.sclamp) {
        b.mac(MAX, temp(kPrev), cnst(0, "xxxx"), Op(), prev);
        b.mac(MIN, temp(kPrev), cnst(0, "yyyy"), Op(), prev);
      }
    }

    // Results to their destinations.
    auto store = [&](u8 dst, u8 mask, Op value) {
      if (!mask) return;
      Builder::Dest d;
      if (in.export_data) {
        d = export_dest(dst, mask);
        if (d.temp < 0 && d.output < 0) return;  // an interpolator we don't use
      } else {
        if (dst > 3) return b.fail();
        d = b.to_temp(dst, mask);
      }
      if (d.output == kOutT0) {
        // Texture coordinates: scaled by constant 0's z, w (1 for ordinary
        // textures, the size in pixels for a render target sampled as a
        // linear texture), and copied to all four texture units for the
        // multi-texture sums of the glow passes.
        for (u8 unit = 0; unit < 4; ++unit) {
          Builder::Dest t = d;
          t.output = (u8)(kOutT0 + unit);
          b.mac(MUL, value, cnst(0, "zwyy"), Op(), t);
        }
        return;
      }
      b.mov(d, value);
    };
    if (vector_used) store(in.vdst, in.vmask, temp(kStage));
    if (scalar_writes) store(in.sdst, in.smask, temp(kPrev));
  }

  // Epilogue: position -> screen (constant 3 = scale x, y, offset x, y).
  Builder::Dest pos_xy;
  pos_xy.output = kOutPos;
  pos_xy.out_mask = 3;
  if (divide) {
    b.ilu(RCC, temp(kPos, "wwww"), b.to_temp(kS0, 1));
    b.mac(MUL, temp(kPos), temp(kS0, "xxxx"), Op(), b.to_temp(kS1, 3));
    b.mac(MAD, temp(kS1), cnst(3, "xyxy"), cnst(3, "zwzw"), pos_xy);
  } else {
    b.mac(MAD, temp(kPos), cnst(3, "xyxy"), cnst(3, "zwzw"), pos_xy);
  }
  Builder::Dest pos_zw;
  pos_zw.output = kOutPos;
  pos_zw.out_mask = 0xC;
  b.mov(pos_zw, cnst(0, "xxxy"));

  if (b.failed || !out.stride) {
    gw_log("nv2a: vertex shader %08X stays on the CPU\n", p.hash);
    out.ok = false;
    return nullptr;
  }
  b.words[(b.count - 1) * 4 + 3] |= 1;  // last instruction
  memcpy(out.words, b.words, b.count * 16);
  out.instructions = (u32)b.count;
  out.ok = true;
  gw_log("nv2a: vertex shader %08X -> %u NV2A instructions, %u constants, %u inputs\n", p.hash, out.instructions,
         out.const_count, out.attr_count);
  return &out;
}
