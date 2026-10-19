/*
 * Copyright © Microsoft Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/* Port notes (uwp-26.2.2): nir_dest/nir_ssa_def/nir_register are gone, so
 * every value is a nir_def and phi webs come back from nir_convert_from_ssa
 * as decl_reg/load_reg/store_reg intrinsics. load_ubo_dxil became
 * load_ubo_vec4, discard became terminate/demote, scoped_barrier became
 * barrier, and the signature and sysvalue helpers live in dxil_signature.c
 * and here instead of the MR's dxil_nir.c additions. */

#include "nir_to_dxbc.h"

#include "util/blob.h"
#include "dxil_container.h"
#include "dxil_enums.h"
#include "dxil_module.h"
#include "dxil_signature.h"
#include "dxil_nir.h"
#include "nir.h"
#include "nir/nir_builder.h"
#include "nir_intrinsics.h"
#include "nir_opcodes.h"
#include "util/ralloc.h"
#include "util/u_debug.h"
#include "util/u_dynarray.h"
#include "util/u_math.h"

#include "ShaderBinary.h"
#include <directx/D3D12TokenizedProgramFormat.hpp>

#include <stdint.h>
#include <vector>
#include <memory>
#include <algorithm>

using namespace D3D10ShaderBinary;

static int debug_dxbc = 0;

enum dxbc_debug_flags {
   DXBC_DEBUG_VERBOSE = 1 << 0,
   DXBC_DEBUG_TRACE = 1 << 2,
};

static const struct debug_named_value
dxbc_debug_options[] = {
   { "verbose", DXBC_DEBUG_VERBOSE, NULL },
   { "trace",  DXBC_DEBUG_TRACE , "Trace instruction conversion" },
   DEBUG_NAMED_VALUE_END
};

DEBUG_GET_ONCE_FLAGS_OPTION(debug_dxbc, "DXBC_DEBUG", dxbc_debug_options, 0)

struct ntd_ssa {
   COperandBase operand;
   int instruction_index = -1;
};

struct DxbcModule {
   D3D10_SB_TOKENIZED_PROGRAM_TYPE shader_kind;
   uint32_t major_version;
   uint32_t minor_version;
   CShaderAsm shader;

   std::unique_ptr<ntd_ssa[]> ssa_operands;
   std::vector<CInstruction> instructions;

   uint32_t reg_alloc;
};

struct ntd_context {
   void *ralloc_ctx;
   const struct nir_to_dxil_options *opts{};
   struct nir_shader *shader{};
   DxbcModule mod{};
   dxil_module dxil_mod{};
   nir_variable *system_value[SYSTEM_VALUE_MAX];
   char *err;
   size_t errlen;
   bool vulkan;

   ntd_context()
   {
      ralloc_ctx = ralloc_context(NULL);
      if (ralloc_ctx)
         dxil_module_init(&dxil_mod, ralloc_ctx);
   }
   ~ntd_context()
   {
      dxil_module_release(&dxil_mod);
      ralloc_free(ralloc_ctx);
   }
};

static void
fail(ntd_context *ctx, const char *what, nir_instr *instr)
{
   if (!ctx->err || ctx->err[0])
      return;
   char *s = NULL;
   if (instr)
      s = nir_instr_as_str(instr, NULL);
   snprintf(ctx->err, ctx->errlen, "%s%s%s", what, s ? ": " : "", s ? s : "");
   ralloc_free(s);
}

#define TRACE_CONVERSION(instr)                                              \
   if (debug_dxbc & DXBC_DEBUG_TRACE)                                        \
      do {                                                                   \
         fprintf(stderr, "Convert '");                                       \
         nir_print_instr(instr, stderr);                                     \
         fprintf(stderr, "'\n");                                             \
   } while (0)

class ScopedDxilContainer {
 public:
   ScopedDxilContainer() { dxil_container_init(&inner); }
   ~ScopedDxilContainer() { dxil_container_finish(&inner); }
   dxil_container *
   get()
   {
      return &inner;
   }

 private:
   dxil_container inner{};
};

static bool
is_sm51(ntd_context *ctx)
{
   return ctx->mod.major_version == 5 && ctx->mod.minor_version == 1;
}

static COperandDst
def_as_register(ntd_context *ctx, const nir_def *def, uint32_t write_mask)
{
   (void)def;
   unsigned index = ctx->mod.reg_alloc++;
   return COperandDst(D3D10_SB_OPERAND_TYPE_TEMP, index,
       write_mask << D3D10_SB_OPERAND_4_COMPONENT_MASK_SHIFT);
}

static uint32_t
full_mask(const nir_def *def)
{
   return (1u << def->num_components) - 1;
}

static COperandBase
nir_src_as_const_value_or_register(ntd_context *ctx, nir_src src, uint32_t num_write_components,
                                   const uint8_t swizzle[4])
{
   uint32_t num_components =
       std::min(num_write_components, nir_src_num_components(src));

   COperandBase ret = ctx->mod.ssa_operands[src.ssa->index].operand;

   // For derefs, the num_components is about the resulting resource access,
   // not about the number of components in the deref itself.
   nir_deref_instr *deref = nir_src_as_deref(src);
   if (deref)
      num_components = 4;

   if (num_components == 1) {
      // Already a single component
      if (ret.m_NumComponents == D3D10_SB_OPERAND_1_COMPONENT) {
         assert(!swizzle || swizzle[0] == 0);
         return ret;
      }

      D3D10_SB_4_COMPONENT_NAME component = D3D10_SB_4_COMPONENT_X;
      if (swizzle)
         component = static_cast<D3D10_SB_4_COMPONENT_NAME>(swizzle[0]);

      // Re-swizzle the constant to be a single component
      if (ret.m_Type == D3D10_SB_OPERAND_TYPE_IMMEDIATE32 || ret.m_Type == D3D10_SB_OPERAND_TYPE_IMMEDIATE64) {
         ret.m_NumComponents = D3D10_SB_OPERAND_1_COMPONENT;
         if (ret.m_Type == D3D10_SB_OPERAND_TYPE_IMMEDIATE32 && component > 0)
            ret.m_Value[0] = ret.m_Value[component];
         else if (component > 0)
            ret.m_Value64[0] = ret.m_Value64[component];
         return ret;
      }

      // Switch to single-component selection
      ret.SelectComponent(static_cast<D3D10_SB_4_COMPONENT_NAME>(ret.SwizzleComponent(component)));
      return ret;
   }

   if (ret.m_NumComponents == D3D10_SB_OPERAND_1_COMPONENT) {
      // A scalar immediate used as a vector: splat it
      if (ret.m_Type == D3D10_SB_OPERAND_TYPE_IMMEDIATE32) {
         for (unsigned i = 1; i < 4; ++i)
            ret.m_Value[i] = ret.m_Value[0];
         ret.m_NumComponents = D3D10_SB_OPERAND_4_COMPONENT;
         ret.SetSwizzle(0, 0, 0, 0);
         return ret;
      }
      assert(!"Don't know how to promote from 1 to 4");
   }

   if (swizzle) {
      uint8_t final_swizzle[4];
      for (unsigned i = 0; i < 4; ++i)
         final_swizzle[i] = ret.SwizzleComponent(swizzle[i]);
      ret.SetSwizzle(final_swizzle[0], final_swizzle[1], final_swizzle[2], final_swizzle[3]);
   }

   return ret;
}

static uint32_t
count_write_components(uint32_t write_mask)
{
   return util_bitcount(write_mask);
}

static COperandBase
get_alu_src_as_const_value_or_register(ntd_context *ctx, nir_alu_instr *alu, int num_components, int src_index)
{
   return nir_src_as_const_value_or_register(ctx, alu->src[src_index].src,
      num_components, alu->src[src_index].swizzle);
}

static CInstruction
get_intr_1_args(ntd_context *ctx, D3D10_SB_OPCODE_TYPE opcode, nir_alu_instr *alu)
{
   uint32_t num_write_components = alu->def.num_components;
   COperandBase dst = def_as_register(ctx, &alu->def, full_mask(&alu->def));
   COperandBase a = get_alu_src_as_const_value_or_register(ctx, alu, num_write_components, 0);
   return CInstruction(opcode, dst, a);
}

static CInstruction
get_intr_2_args(ntd_context *ctx, D3D10_SB_OPCODE_TYPE opcode, nir_alu_instr *alu)
{
   uint32_t num_write_components = alu->def.num_components;
   COperandBase dst = def_as_register(ctx, &alu->def, full_mask(&alu->def));
   COperandBase lhs = get_alu_src_as_const_value_or_register(ctx, alu, num_write_components, 0);
   COperandBase rhs = get_alu_src_as_const_value_or_register(ctx, alu, num_write_components, 1);
   return CInstruction(opcode, dst, lhs, rhs);
}

static CInstruction
get_intr_dot_product(ntd_context *ctx, D3D10_SB_OPCODE_TYPE opcode, nir_alu_instr *alu)
{
   uint32_t num_src_components = alu->op == nir_op_fdot4 ?
      4 : (alu->op == nir_op_fdot3 ? 3 : 2);
   COperandBase dst = def_as_register(ctx, &alu->def, full_mask(&alu->def));
   COperandBase lhs = get_alu_src_as_const_value_or_register(ctx, alu, num_src_components, 0);
   COperandBase rhs = get_alu_src_as_const_value_or_register(ctx, alu, num_src_components, 1);
   return CInstruction(opcode, dst, lhs, rhs);
}

static CInstruction
get_intr_3_args(ntd_context *ctx, D3D10_SB_OPCODE_TYPE opcode, nir_alu_instr *alu)
{
   uint32_t num_write_components = alu->def.num_components;
   COperandBase dst = def_as_register(ctx, &alu->def, full_mask(&alu->def));
   COperandBase arg0 = get_alu_src_as_const_value_or_register(ctx, alu, num_write_components, 0);
   COperandBase arg1 = get_alu_src_as_const_value_or_register(ctx, alu, num_write_components, 1);
   COperandBase arg2 = get_alu_src_as_const_value_or_register(ctx, alu, num_write_components, 2);
   return CInstruction(opcode, dst, arg0, arg1, arg2);
}

static CInstruction
get_intr_4_args(ntd_context *ctx, D3D10_SB_OPCODE_TYPE opcode, nir_alu_instr *alu)
{
   uint32_t num_write_components = alu->def.num_components;
   COperandBase dst = def_as_register(ctx, &alu->def, full_mask(&alu->def));
   COperandBase arg0 = get_alu_src_as_const_value_or_register(ctx, alu, num_write_components, 0);
   COperandBase arg1 = get_alu_src_as_const_value_or_register(ctx, alu, num_write_components, 1);
   COperandBase arg2 = get_alu_src_as_const_value_or_register(ctx, alu, num_write_components, 2);
   COperandBase arg3 = get_alu_src_as_const_value_or_register(ctx, alu, num_write_components, 3);
   return CInstruction(opcode, dst, arg0, arg1, arg2, arg3);
}

/* Records `instr` as the producer of `dest`; the first TEMP operand of the
 * instruction is the register that holds the value. */
static void
store_instruction(ntd_context *ctx, nir_def *dest, CInstruction const& instr)
{
   ctx->mod.ssa_operands[dest->index].instruction_index = (int)ctx->mod.instructions.size();
   ctx->mod.instructions.push_back(instr);
   for (unsigned i = 0; i < instr.m_NumOperands; ++i) {
      const COperandBase &instr_dest = instr.Operand(i);
      if (instr_dest.m_Type == D3D10_SB_OPERAND_TYPE_NULL)
         continue;
      assert(instr_dest.m_Type == D3D10_SB_OPERAND_TYPE_TEMP);
      assert(instr_dest.m_IndexDimension == D3D10_SB_OPERAND_INDEX_1D);
      assert(instr_dest.m_IndexType[0] == D3D10_SB_OPERAND_INDEX_IMMEDIATE32);
      assert(instr_dest.m_ComponentSelection == D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE);
      ctx->mod.ssa_operands[dest->index].operand = COperand4(instr_dest.m_Type, instr_dest.RegIndex(0));
      return;
   }
   UNREACHABLE("Instruction has no destination");
}

static bool
modify_ssa_instruction(ntd_context *ctx, nir_alu_instr *alu, void (*callback)(CInstruction&))
{
   if (list_length(&alu->src[0].src.ssa->uses) == 1) {
      // Modify the previous instruction to saturate
      auto& prev_ssa_src = ctx->mod.ssa_operands[alu->src[0].src.ssa->index];
      if (prev_ssa_src.instruction_index >= 0) {
         CInstruction& prev_intr = ctx->mod.instructions[prev_ssa_src.instruction_index];
         callback(prev_intr);
         ctx->mod.ssa_operands[alu->def.index].operand =
            get_alu_src_as_const_value_or_register(ctx, alu, alu->def.num_components, 0);
         return true;
      }
   }
   return false;
}

static COperand4
new_temp(ntd_context *ctx)
{
   return COperand4(D3D10_SB_OPERAND_TYPE_TEMP, ctx->mod.reg_alloc++);
}

static COperandDst
temp_dst(const COperand4 &t, uint32_t mask)
{
   return COperandDst(D3D10_SB_OPERAND_TYPE_TEMP, t.RegIndex(0), mask << D3D10_SB_OPERAND_4_COMPONENT_MASK_SHIFT);
}

static bool
emit_alu(struct ntd_context *ctx, nir_alu_instr *alu)
{
   COperand null_operand(D3D10_SB_OPERAND_TYPE_NULL);
   uint32_t mask = full_mask(&alu->def);
   unsigned ncomp = alu->def.num_components;

   switch (alu->op) {
   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4: {
      // TODO: coalesce like-sourced `mov`s together
      COperandDst dst = def_as_register(ctx, &alu->def, 0);
      for (unsigned chan = 0; chan < alu->def.num_components; chan++) {
         dst.SetMask((1 << chan) << D3D10_SB_OPERAND_4_COMPONENT_MASK_SHIFT);
         COperandBase src = get_alu_src_as_const_value_or_register(ctx, alu, 1, chan);
         CInstruction mov(D3D10_SB_OPCODE_MOV, dst, src);
         ctx->mod.instructions.push_back(mov);
      }
      ctx->mod.ssa_operands[alu->def.index].operand =
         COperand4(D3D10_SB_OPERAND_TYPE_TEMP, dst.RegIndex(0));
      return true;
   }

   case nir_op_fmax:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_MAX, alu));
      return true;
   case nir_op_fmin:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_MIN, alu));
      return true;

   case nir_op_fsat: {
      if (!modify_ssa_instruction(ctx, alu, [](CInstruction &prev_intr) { prev_intr.m_bSaturate = true; })) {
         CInstruction intr = get_intr_1_args(ctx, D3D10_SB_OPCODE_MOV, alu);
         intr.m_bSaturate = true;
         store_instruction(ctx, &alu->def, intr);
      }
      return true;
   }

   case nir_op_fmul:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_MUL, alu));
      return true;
   case nir_op_fdiv:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_DIV, alu));
      return true;
   case nir_op_fadd:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_ADD, alu));
      return true;
   case nir_op_ffma:
      store_instruction(ctx, &alu->def, get_intr_3_args(ctx, D3D10_SB_OPCODE_MAD, alu));
      return true;

   case nir_op_fsub: {
      CInstruction intr = get_intr_2_args(ctx, D3D10_SB_OPCODE_ADD, alu);
      if (intr.m_Operands[2].m_Type == D3D10_SB_OPERAND_TYPE_IMMEDIATE32) {
         unsigned num_components = intr.m_Operands[2].m_NumComponents == D3D10_SB_OPERAND_4_COMPONENT ? 4 : 1;
         for (unsigned i = 0; i < num_components; ++i)
            intr.m_Operands[2].m_Valuef[i] = -intr.m_Operands[2].m_Valuef[i];
      } else
         intr.m_Operands[2].SetModifier(D3D10_SB_OPERAND_MODIFIER_NEG);
      store_instruction(ctx, &alu->def, intr);
      return true;
   }

   case nir_op_fneg: {
      CInstruction intr = get_intr_1_args(ctx, D3D10_SB_OPCODE_MOV, alu);
      if (intr.m_Operands[1].m_Type == D3D10_SB_OPERAND_TYPE_IMMEDIATE32) {
         unsigned num_components = intr.m_Operands[1].m_NumComponents == D3D10_SB_OPERAND_4_COMPONENT ? 4 : 1;
         for (unsigned i = 0; i < num_components; ++i)
            intr.m_Operands[1].m_Valuef[i] = -intr.m_Operands[1].m_Valuef[i];
      } else
         intr.m_Operands[1].SetModifier(D3D10_SB_OPERAND_MODIFIER_NEG);
      store_instruction(ctx, &alu->def, intr);
      return true;
   }

   case nir_op_bcsel:
   case nir_op_b32csel:
      store_instruction(ctx, &alu->def, get_intr_3_args(ctx, D3D10_SB_OPCODE_MOVC, alu));
      return true;

   case nir_op_feq32:
   case nir_op_feq:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_EQ, alu));
      return true;
   case nir_op_fneu32:
   case nir_op_fneu:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_NE, alu));
      return true;
   case nir_op_flt32:
   case nir_op_flt:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_LT, alu));
      return true;
   case nir_op_fge32:
   case nir_op_fge:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_GE, alu));
      return true;

   case nir_op_mov:
      ctx->mod.ssa_operands[alu->def.index].operand =
         get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 0);
      return true;

   case nir_op_imax:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_IMAX, alu));
      return true;
   case nir_op_imin:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_IMIN, alu));
      return true;
   case nir_op_ige32:
   case nir_op_ige:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_IGE, alu));
      return true;
   case nir_op_ilt32:
   case nir_op_ilt:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_ILT, alu));
      return true;
   case nir_op_ieq32:
   case nir_op_ieq:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_IEQ, alu));
      return true;
   case nir_op_ine32:
   case nir_op_ine:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_INE, alu));
      return true;
   case nir_op_ior:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_OR, alu));
      return true;
   case nir_op_iand:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_AND, alu));
      return true;
   case nir_op_ixor:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_XOR, alu));
      return true;
   case nir_op_iadd:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_IADD, alu));
      return true;

   case nir_op_imul: {
      COperandBase dest = def_as_register(ctx, &alu->def, mask);
      COperandBase src0 = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 0);
      COperandBase src1 = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 1);
      store_instruction(ctx, &alu->def, CInstruction(D3D10_SB_OPCODE_IMUL, null_operand, dest, src0, src1));
      return true;
   }
   case nir_op_imul_high:
   case nir_op_umul_high: {
      COperandBase dest = def_as_register(ctx, &alu->def, mask);
      COperandBase src0 = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 0);
      COperandBase src1 = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 1);
      store_instruction(ctx, &alu->def, CInstruction(alu->op == nir_op_imul_high ? D3D10_SB_OPCODE_IMUL : D3D10_SB_OPCODE_UMUL,
                                                     dest, null_operand, src0, src1));
      return true;
   }

   case nir_op_isub: {
      CInstruction intr = get_intr_2_args(ctx, D3D10_SB_OPCODE_IADD, alu);
      if (intr.m_Operands[2].m_Type == D3D10_SB_OPERAND_TYPE_IMMEDIATE32) {
         unsigned num_components = intr.m_Operands[2].m_NumComponents == D3D10_SB_OPERAND_4_COMPONENT ? 4 : 1;
         for (unsigned i = 0; i < num_components; ++i)
            intr.m_Operands[2].m_Value[i] = (UINT)(-(INT)intr.m_Operands[2].m_Value[i]);
      } else
         intr.m_Operands[2].SetModifier(D3D10_SB_OPERAND_MODIFIER_NEG);
      store_instruction(ctx, &alu->def, intr);
      return true;
   }
   case nir_op_ineg: {
      CInstruction intr = get_intr_1_args(ctx, D3D10_SB_OPCODE_MOV, alu);
      if (intr.m_Operands[1].m_Type == D3D10_SB_OPERAND_TYPE_IMMEDIATE32) {
         unsigned num_components = intr.m_Operands[1].m_NumComponents == D3D10_SB_OPERAND_4_COMPONENT ? 4 : 1;
         for (unsigned i = 0; i < num_components; ++i)
            intr.m_Operands[1].m_Value[i] = (UINT)(-(INT)intr.m_Operands[1].m_Value[i]);
         store_instruction(ctx, &alu->def, intr);
      } else {
         intr.m_OpCode = D3D10_SB_OPCODE_INEG;
         store_instruction(ctx, &alu->def, intr);
      }
      return true;
   }

   case nir_op_umax:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_UMAX, alu));
      return true;
   case nir_op_umin:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_UMIN, alu));
      return true;

   case nir_op_udiv:
   case nir_op_umod: {
      COperandBase dst0 = def_as_register(ctx, &alu->def, mask);
      COperandBase dst1 = null_operand;
      if (alu->op == nir_op_umod)
         std::swap(dst0, dst1);
      COperandBase src0 = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 0);
      COperandBase src1 = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 1);
      store_instruction(ctx, &alu->def, CInstruction(D3D10_SB_OPCODE_UDIV, dst0, dst1, src0, src1));
      return true;
   }

   case nir_op_idiv:
   case nir_op_irem:
   case nir_op_imod: {
      /* No signed divide in SM5: udiv on magnitudes, then fix the sign. */
      COperandBase a = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 0);
      COperandBase b = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 1);
      COperand4 absa = new_temp(ctx), absb = new_temp(ctx), q = new_temp(ctx), r = new_temp(ctx), sgn = new_temp(ctx), res = new_temp(ctx);
      COperandBase nega = a; nega.SetModifier(D3D10_SB_OPERAND_MODIFIER_NEG);
      COperandBase negb = b; negb.SetModifier(D3D10_SB_OPERAND_MODIFIER_NEG);
      ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_IMAX, temp_dst(absa, mask), a, nega));
      ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_IMAX, temp_dst(absb, mask), b, negb));
      ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_UDIV, temp_dst(q, mask), temp_dst(r, mask), absa, absb));
      /* sgn = (a ^ b) < 0 for the quotient, a < 0 for the remainder */
      COperand zero(0u);
      if (alu->op == nir_op_idiv) {
         ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_XOR, temp_dst(sgn, mask), a, b));
         ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_ILT, temp_dst(sgn, mask), sgn, zero));
         COperandBase negq = q; negq.SetModifier(D3D10_SB_OPERAND_MODIFIER_NEG);
         ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_MOVC, temp_dst(res, mask), sgn, negq, q));
      } else {
         ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_ILT, temp_dst(sgn, mask), a, zero));
         COperandBase negr = r; negr.SetModifier(D3D10_SB_OPERAND_MODIFIER_NEG);
         ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_MOVC, temp_dst(res, mask), sgn, negr, r));
         if (alu->op == nir_op_imod) {
            /* imod: if result != 0 and sign(result) != sign(b), add b */
            COperand4 t = new_temp(ctx), u = new_temp(ctx);
            ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_XOR, temp_dst(t, mask), res, b));
            ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_ILT, temp_dst(t, mask), t, zero));
            ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_INE, temp_dst(u, mask), res, zero));
            ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_AND, temp_dst(t, mask), t, u));
            ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_IADD, temp_dst(u, mask), res, b));
            ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_MOVC, temp_dst(res, mask), t, u, res));
         }
      }
      ctx->mod.ssa_operands[alu->def.index].operand = res;
      return true;
   }

   case nir_op_ushr:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_USHR, alu));
      return true;
   case nir_op_ishr:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_ISHR, alu));
      return true;
   case nir_op_ishl:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_ISHL, alu));
      return true;

   case nir_op_fsqrt:
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D10_SB_OPCODE_SQRT, alu));
      return true;
   case nir_op_frsq:
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D10_SB_OPCODE_RSQ, alu));
      return true;
   case nir_op_frcp:
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D11_SB_OPCODE_RCP, alu));
      return true;
   case nir_op_fexp2:
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D10_SB_OPCODE_EXP, alu));
      return true;
   case nir_op_flog2:
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D10_SB_OPCODE_LOG, alu));
      return true;
   case nir_op_ffloor:
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D10_SB_OPCODE_ROUND_NI, alu));
      return true;
   case nir_op_fceil:
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D10_SB_OPCODE_ROUND_PI, alu));
      return true;
   case nir_op_fround_even:
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D10_SB_OPCODE_ROUND_NE, alu));
      return true;
   case nir_op_ffract:
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D10_SB_OPCODE_FRC, alu));
      return true;
   case nir_op_ftrunc:
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D10_SB_OPCODE_ROUND_Z, alu));
      return true;

   case nir_op_fabs: {
      auto& operand = ctx->mod.ssa_operands[alu->def.index].operand;
      operand = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 0);
      if (operand.m_Type == D3D10_SB_OPERAND_TYPE_IMMEDIATE32) {
         for (unsigned i = 0; i < 4; ++i)
            operand.m_Valuef[i] = fabsf(operand.m_Valuef[i]);
      } else
         operand.SetModifier(D3D10_SB_OPERAND_MODIFIER_ABS);
      return true;
   }

   case nir_op_f2i32:
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D10_SB_OPCODE_FTOI, alu));
      return true;
   case nir_op_i2f32:
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D10_SB_OPCODE_ITOF, alu));
      return true;
   case nir_op_u2f32:
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D10_SB_OPCODE_UTOF, alu));
      return true;
   case nir_op_f2u32:
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D10_SB_OPCODE_FTOU, alu));
      return true;
   case nir_op_b2f32: {
      /* bools are 0 / ~0 after nir_lower_bool_to_int32 */
      COperandBase dst = def_as_register(ctx, &alu->def, mask);
      COperandBase a = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 0);
      COperand one(1.0f);
      store_instruction(ctx, &alu->def, CInstruction(D3D10_SB_OPCODE_AND, dst, a, one));
      return true;
   }
   case nir_op_b2i32: {
      COperandBase dst = def_as_register(ctx, &alu->def, mask);
      COperandBase a = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 0);
      COperand one(1u);
      store_instruction(ctx, &alu->def, CInstruction(D3D10_SB_OPCODE_AND, dst, a, one));
      return true;
   }

   case nir_op_fcos: {
      COperandBase dst = def_as_register(ctx, &alu->def, mask);
      COperandBase a = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 0);
      store_instruction(ctx, &alu->def, CInstruction(D3D10_SB_OPCODE_SINCOS, null_operand, dst, a));
      return true;
   }
   case nir_op_fsin: {
      COperandBase dst = def_as_register(ctx, &alu->def, mask);
      COperandBase a = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 0);
      store_instruction(ctx, &alu->def, CInstruction(D3D10_SB_OPCODE_SINCOS, dst, null_operand, a));
      return true;
   }

   case nir_op_fddx:
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D10_SB_OPCODE_DERIV_RTX, alu));
      return true;
   case nir_op_fddy:
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D10_SB_OPCODE_DERIV_RTY, alu));
      return true;
   case nir_op_fddx_fine:
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D11_SB_OPCODE_DERIV_RTX_FINE, alu));
      return true;
   case nir_op_fddy_fine:
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D11_SB_OPCODE_DERIV_RTY_FINE, alu));
      return true;
   case nir_op_fddx_coarse:
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D11_SB_OPCODE_DERIV_RTX_COARSE, alu));
      return true;
   case nir_op_fddy_coarse:
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D11_SB_OPCODE_DERIV_RTY_COARSE, alu));
      return true;

   case nir_op_fdot4:
      store_instruction(ctx, &alu->def, get_intr_dot_product(ctx, D3D10_SB_OPCODE_DP4, alu));
      return true;
   case nir_op_fdot3:
      store_instruction(ctx, &alu->def, get_intr_dot_product(ctx, D3D10_SB_OPCODE_DP3, alu));
      return true;
   case nir_op_fdot2:
      store_instruction(ctx, &alu->def, get_intr_dot_product(ctx, D3D10_SB_OPCODE_DP2, alu));
      return true;

   case nir_op_ult32:
   case nir_op_ult:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_ULT, alu));
      return true;
   case nir_op_uge32:
   case nir_op_uge:
      store_instruction(ctx, &alu->def, get_intr_2_args(ctx, D3D10_SB_OPCODE_UGE, alu));
      return true;

   case nir_op_inot: {
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D10_SB_OPCODE_NOT, alu));
      return true;
   }

   case nir_op_ubfe:
   case nir_op_ibfe: {
      /* NIR: (value, offset, bits); DXBC: (width, offset, value) */
      COperandBase dst = def_as_register(ctx, &alu->def, mask);
      COperandBase v = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 0);
      COperandBase o = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 1);
      COperandBase w = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 2);
      store_instruction(ctx, &alu->def, CInstruction(alu->op == nir_op_ubfe ? D3D11_SB_OPCODE_UBFE : D3D11_SB_OPCODE_IBFE, dst, w, o, v));
      return true;
   }
   case nir_op_bitfield_insert: {
      /* NIR: (base, insert, offset, bits); DXBC bfi: (width, offset, insert, base) */
      COperandBase dst = def_as_register(ctx, &alu->def, mask);
      COperandBase base = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 0);
      COperandBase ins = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 1);
      COperandBase o = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 2);
      COperandBase w = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 3);
      store_instruction(ctx, &alu->def, CInstruction(D3D11_SB_OPCODE_BFI, dst, w, o, ins, base));
      return true;
   }
   case nir_op_bit_count:
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D11_SB_OPCODE_COUNTBITS, alu));
      return true;
   case nir_op_bitfield_reverse:
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D11_SB_OPCODE_BFREV, alu));
      return true;
   case nir_op_find_lsb:
      store_instruction(ctx, &alu->def, get_intr_1_args(ctx, D3D11_SB_OPCODE_FIRSTBIT_LO, alu));
      return true;
   case nir_op_ufind_msb:
   case nir_op_ifind_msb: {
      /* firstbit_hi counts from the MSB; NIR wants the bit index, -1 for zero */
      COperandBase a = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 0);
      COperand4 t = new_temp(ctx), c = new_temp(ctx), res = new_temp(ctx);
      ctx->mod.instructions.push_back(CInstruction(alu->op == nir_op_ufind_msb ? D3D11_SB_OPCODE_FIRSTBIT_HI : D3D11_SB_OPCODE_FIRSTBIT_SHI, temp_dst(t, mask), a));
      COperand all(UINT_MAX), thirtyone(31u);
      ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_IEQ, temp_dst(c, mask), t, all));
      COperandBase negt = t; negt.SetModifier(D3D10_SB_OPERAND_MODIFIER_NEG);
      ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_IADD, temp_dst(res, mask), negt, thirtyone));
      ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_MOVC, temp_dst(res, mask), c, all, res));
      ctx->mod.ssa_operands[alu->def.index].operand = res;
      return true;
   }

   case nir_op_pack_half_2x16_split: {
      COperandBase lo = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 0);
      COperandBase hi = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 1);
      COperand4 a = new_temp(ctx), b = new_temp(ctx), res = new_temp(ctx);
      ctx->mod.instructions.push_back(CInstruction(D3D11_SB_OPCODE_F32TOF16, temp_dst(a, mask), lo));
      ctx->mod.instructions.push_back(CInstruction(D3D11_SB_OPCODE_F32TOF16, temp_dst(b, mask), hi));
      COperand sixteen(16u);
      ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_ISHL, temp_dst(b, mask), b, sixteen));
      ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_OR, temp_dst(res, mask), a, b));
      ctx->mod.ssa_operands[alu->def.index].operand = res;
      return true;
   }
   case nir_op_unpack_half_2x16_split_x:
   case nir_op_unpack_half_2x16_split_y: {
      COperandBase a = get_alu_src_as_const_value_or_register(ctx, alu, ncomp, 0);
      COperand4 t = new_temp(ctx), res = new_temp(ctx);
      if (alu->op == nir_op_unpack_half_2x16_split_y) {
         COperand sixteen(16u);
         ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_USHR, temp_dst(t, mask), a, sixteen));
         a = t;
      }
      ctx->mod.instructions.push_back(CInstruction(D3D11_SB_OPCODE_F16TOF32, temp_dst(res, mask), a));
      ctx->mod.ssa_operands[alu->def.index].operand = res;
      return true;
   }

   default:
      fail(ctx, "unsupported ALU op", &alu->instr);
      return false;
   }
}

static void
src_to_index(ntd_context *ctx, unsigned const_base, nir_src src, COperandBase& op, int index_to_set)
{
   D3D10_SB_4_COMPONENT_NAME component = D3D10_SB_4_COMPONENT_X;

   COperandBase& src_operand = ctx->mod.ssa_operands[src.ssa->index].operand;
   if (src_operand.m_ComponentSelection == D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE)
      component = src_operand.m_ComponentName;
   else if (src_operand.m_ComponentSelection == D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE)
      component = static_cast<D3D10_SB_4_COMPONENT_NAME>(src_operand.SwizzleComponent(0));
   if (src_operand.m_Type == D3D10_SB_OPERAND_TYPE_IMMEDIATE32) {
      op.SetIndex(index_to_set, const_base + src_operand.m_Value[src_operand.m_NumComponents == D3D10_SB_OPERAND_1_COMPONENT ? 0 : component]);
   } else if (src_operand.m_Type == D3D10_SB_OPERAND_TYPE_TEMP &&
              src_operand.m_IndexDimension == D3D10_SB_OPERAND_INDEX_1D &&
              src_operand.m_IndexType[0] == D3D10_SB_OPERAND_INDEX_IMMEDIATE32) {
      op.SetIndex(index_to_set, const_base, src_operand.m_Type, src_operand.RegIndex(0), 0, component);
   } else {
      // Load from the source into a temp, we can't use the value as an index directly
      COperandDst dst(D3D10_SB_OPERAND_TYPE_TEMP, ctx->mod.reg_alloc++, D3D10_SB_OPERAND_4_COMPONENT_MASK_X);
      COperandBase one = src_operand;
      if (one.m_NumComponents == D3D10_SB_OPERAND_4_COMPONENT)
         one.SelectComponent(component);
      ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_MOV, dst, one));
      op.SetIndex(index_to_set, const_base, dst.m_Type, dst.RegIndex(0), 0, D3D10_SB_4_COMPONENT_X);
   }
}

static unsigned
io_register(ntd_context *ctx, nir_intrinsic_instr *intr, bool output)
{
   unsigned base = nir_intrinsic_base(intr);
   const uint8_t *map = output ? ctx->dxil_mod.output_mappings : ctx->dxil_mod.input_mappings;
   const struct dxil_psv_signature_element *psv = output ? ctx->dxil_mod.psv_outputs : ctx->dxil_mod.psv_inputs;
   return psv[map[base]].start_row;
}

static COperandBase
get_output_operand_type(struct ntd_context *ctx, nir_intrinsic_instr *intr)
{
   unsigned component = nir_intrinsic_has_component(intr) ? nir_intrinsic_component(intr) : 0;
   unsigned write_mask = (nir_intrinsic_write_mask(intr) << component) << D3D10_SB_OPERAND_4_COMPONENT_MASK_SHIFT;
   nir_io_semantics semantics = nir_intrinsic_io_semantics(intr);
   if (ctx->mod.shader_kind == D3D10_SB_PIXEL_SHADER) {
      switch (semantics.location) {
      case FRAG_RESULT_DEPTH:
         return COperandDst(write_mask, D3D10_SB_OPERAND_TYPE_OUTPUT_DEPTH);
      case FRAG_RESULT_STENCIL:
         return COperandDst(write_mask, D3D11_SB_OPERAND_TYPE_OUTPUT_STENCIL_REF);
      default:
         break;
      }
   }

   COperandDst dst(D3D10_SB_OPERAND_TYPE_OUTPUT, 0, write_mask);
   src_to_index(ctx, io_register(ctx, intr, true), intr->src[1], dst, 0);
   return dst;
}

static bool
emit_store_output(struct ntd_context *ctx, nir_intrinsic_instr *intr)
{
   unsigned component = nir_intrinsic_has_component(intr) ? nir_intrinsic_component(intr) : 0;
   COperandBase src = nir_src_as_const_value_or_register(ctx, intr->src[0], 4, nullptr);
   if (component && src.m_NumComponents == D3D10_SB_OPERAND_4_COMPONENT &&
       src.m_ComponentSelection == D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE) {
      /* column c of the output reads source component c - component */
      uint8_t sw[4];
      for (unsigned c = 0; c < 4; ++c)
         sw[c] = src.SwizzleComponent(c >= component ? c - component : 0);
      src.SetSwizzle(sw[0], sw[1], sw[2], sw[3]);
   } else if (component && src.m_Type == D3D10_SB_OPERAND_TYPE_IMMEDIATE32 &&
              src.m_NumComponents == D3D10_SB_OPERAND_4_COMPONENT) {
      UINT v[4];
      for (unsigned c = 0; c < 4; ++c)
         v[c] = src.m_Value[c >= component ? c - component : 0];
      for (unsigned c = 0; c < 4; ++c)
         src.m_Value[c] = v[c];
   }
   COperandBase dst = get_output_operand_type(ctx, intr);
   CInstruction mov(D3D10_SB_OPCODE_MOV, dst, src);
   ctx->mod.instructions.push_back(mov);
   return true;
}

static void
swizzle_input(COperandBase &src, nir_intrinsic_instr *intr)
{
   unsigned num_components = intr->def.num_components;
   unsigned base_component = nir_intrinsic_has_component(intr) ? nir_intrinsic_component(intr) : 0;

   if (num_components == 1)
      src.SelectComponent(static_cast<D3D10_SB_4_COMPONENT_NAME>(base_component));
   else {
      uint8_t swizzle[4] = {};
      for (unsigned i = 0; i < 4; ++i)
         swizzle[i] = base_component + i < 4 ? base_component + i : 3;
      src.SetSwizzle(swizzle[0], swizzle[1], swizzle[2], swizzle[3]);
   }
}

static bool
emit_load_input(struct ntd_context *ctx, nir_intrinsic_instr *intr)
{
   COperand4 src(D3D10_SB_OPERAND_TYPE_INPUT, 0);
   unsigned offset_src = intr->intrinsic == nir_intrinsic_load_interpolated_input ? 1 : 0;
   src_to_index(ctx, io_register(ctx, intr, false), intr->src[offset_src], src, 0);
   swizzle_input(src, intr);
   ctx->mod.ssa_operands[intr->def.index].operand = src;
   return true;
}

static bool
emit_load_per_vertex_input(struct ntd_context *ctx, nir_intrinsic_instr *intr)
{
   COperand2D src(D3D10_SB_OPERAND_TYPE_INPUT, 0, 0);
   src_to_index(ctx, 0, intr->src[0], src, 0);
   src_to_index(ctx, io_register(ctx, intr, false), intr->src[1], src, 1);
   swizzle_input(src, intr);
   ctx->mod.ssa_operands[intr->def.index].operand = src;
   return true;
}

static bool
emit_load_ubo_vec4(struct ntd_context *ctx, nir_intrinsic_instr *intr)
{
   COperandBase src;
   src.m_Type = D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER;
   src.SetSwizzle();
   src.m_NumComponents = D3D10_SB_OPERAND_4_COMPONENT;
   if (is_sm51(ctx)) {
      src.m_IndexDimension = D3D10_SB_OPERAND_INDEX_3D;
      nir_binding binding = nir_chase_binding(intr->src[0]);
      nir_variable *var = nir_get_binding_variable(ctx->shader, binding);
      if (!var) {
         fail(ctx, "cannot find UBO variable for load", &intr->instr);
         return false;
      }
      src.SetIndex(0, var->data.driver_location);
      src_to_index(ctx, ctx->vulkan ? binding.binding : 0, intr->src[0], src, 1);
      src_to_index(ctx, 0, intr->src[1], src, 2);
   } else {
      src.m_IndexDimension = D3D10_SB_OPERAND_INDEX_2D;
      src_to_index(ctx, 0, intr->src[0], src, 0);
      src_to_index(ctx, 0, intr->src[1], src, 1);
   }

   unsigned component = nir_intrinsic_has_component(intr) ? nir_intrinsic_component(intr) : 0;
   if (component) {
      uint8_t sw[4];
      for (unsigned i = 0; i < 4; ++i)
         sw[i] = MIN2(component + i, 3);
      src.SetSwizzle(sw[0], sw[1], sw[2], sw[3]);
   }
   if (intr->def.num_components == 1)
      src.SelectComponent(static_cast<D3D10_SB_4_COMPONENT_NAME>(component));

   if (list_length(&intr->def.uses) == 1)
      ctx->mod.ssa_operands[intr->def.index].operand = src;
   else {
      COperandDst dst = def_as_register(ctx, &intr->def, 0b1111);
      store_instruction(ctx, &intr->def, CInstruction(D3D10_SB_OPCODE_MOV, dst, src));
   }
   return true;
}

static bool
emit_barrier(struct ntd_context *ctx, nir_intrinsic_instr *intr)
{
   CInstruction inst(D3D11_SB_OPCODE_SYNC);

   if (nir_intrinsic_execution_scope(intr) == SCOPE_WORKGROUP)
      inst.m_SyncFlags.bThreadsInGroup = true;

   nir_variable_mode modes = nir_intrinsic_memory_modes(intr);
   mesa_scope mem_scope = nir_intrinsic_memory_scope(intr);

   if (modes & nir_var_uniform)
      modes |= nir_var_mem_global;

   if (modes & (nir_var_mem_ssbo | nir_var_mem_global)) {
      if (mem_scope > SCOPE_WORKGROUP)
         inst.m_SyncFlags.bUnorderedAccessViewMemoryGlobal = true;
      else
         inst.m_SyncFlags.bUnorderedAccessViewMemoryGroup = true;
   }

   if (modes & nir_var_mem_shared)
      inst.m_SyncFlags.bThreadGroupSharedMemory = true;

   if (!inst.m_SyncFlags.bThreadsInGroup && !inst.m_SyncFlags.bUnorderedAccessViewMemoryGlobal &&
       !inst.m_SyncFlags.bUnorderedAccessViewMemoryGroup && !inst.m_SyncFlags.bThreadGroupSharedMemory)
      return true;

   ctx->mod.instructions.push_back(inst);
   return true;
}

static bool
emit_load_system_value(ntd_context *ctx, nir_intrinsic_instr *intr, D3D10_SB_OPERAND_TYPE operand)
{
   COperand op(operand);
   if (op.m_NumComponents == D3D10_SB_OPERAND_4_COMPONENT)
      op.SetSwizzle();
   else {
      // Looks like the helper and at least WARP's DXBC reader disagree here, so
      // follow WARP's expected behavior which probably matches FXC
      op.m_NumComponents = D3D10_SB_OPERAND_4_COMPONENT;
      op.SelectComponent();
   }
   if (list_length(&intr->def.uses) == 1)
      ctx->mod.ssa_operands[intr->def.index].operand = op;
   else {
      COperandDst dst = def_as_register(ctx, &intr->def, full_mask(&intr->def));
      store_instruction(ctx, &intr->def, CInstruction(D3D10_SB_OPCODE_MOV, dst, op));
   }
   return true;
}

static bool
emit_decl_reg(ntd_context *ctx, nir_intrinsic_instr *intr)
{
   ctx->mod.ssa_operands[intr->def.index].operand = new_temp(ctx);
   return true;
}

static bool
emit_load_reg(ntd_context *ctx, nir_intrinsic_instr *intr)
{
   COperandBase reg = ctx->mod.ssa_operands[intr->src[0].ssa->index].operand;
   if (intr->def.num_components == 1)
      reg.SelectComponent(D3D10_SB_4_COMPONENT_X);
   ctx->mod.ssa_operands[intr->def.index].operand = reg;
   return true;
}

static bool
emit_store_reg(ntd_context *ctx, nir_intrinsic_instr *intr)
{
   COperandBase reg = ctx->mod.ssa_operands[intr->src[1].ssa->index].operand;
   COperandDst dst(D3D10_SB_OPERAND_TYPE_TEMP, reg.RegIndex(0),
                   nir_intrinsic_write_mask(intr) << D3D10_SB_OPERAND_4_COMPONENT_MASK_SHIFT);
   COperandBase value = nir_src_as_const_value_or_register(ctx, intr->src[0], 4, nullptr);
   ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_MOV, dst, value));
   return true;
}

static bool
emit_intrinsic(struct ntd_context *ctx, nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_input:
   case nir_intrinsic_load_interpolated_input:
      return emit_load_input(ctx, intr);
   case nir_intrinsic_load_per_vertex_input:
      return emit_load_per_vertex_input(ctx, intr);
   case nir_intrinsic_store_output:
      return emit_store_output(ctx, intr);
   case nir_intrinsic_load_barycentric_pixel:
   case nir_intrinsic_load_barycentric_centroid:
   case nir_intrinsic_load_barycentric_sample:
      return true;

   case nir_intrinsic_decl_reg:
      return emit_decl_reg(ctx, intr);
   case nir_intrinsic_load_reg:
      return emit_load_reg(ctx, intr);
   case nir_intrinsic_store_reg:
      return emit_store_reg(ctx, intr);

   case nir_intrinsic_load_global_invocation_id:
      return emit_load_system_value(ctx, intr, D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID);
   case nir_intrinsic_load_workgroup_id:
      return emit_load_system_value(ctx, intr, D3D11_SB_OPERAND_TYPE_INPUT_THREAD_GROUP_ID);
   case nir_intrinsic_load_local_invocation_id:
      return emit_load_system_value(ctx, intr, D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP);
   case nir_intrinsic_load_local_invocation_index:
      return emit_load_system_value(ctx, intr, D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP_FLATTENED);
   case nir_intrinsic_load_primitive_id:
      return emit_load_system_value(ctx, intr, D3D10_SB_OPERAND_TYPE_INPUT_PRIMITIVEID);

   case nir_intrinsic_load_ubo_vec4:
      return emit_load_ubo_vec4(ctx, intr);

   case nir_intrinsic_barrier:
      return emit_barrier(ctx, intr);

   case nir_intrinsic_end_primitive:
   case nir_intrinsic_emit_vertex: {
      COperand src(D3D11_SB_OPERAND_TYPE_STREAM, nir_intrinsic_stream_id(intr));
      ctx->mod.instructions.push_back(CInstruction(
         intr->intrinsic == nir_intrinsic_emit_vertex ?
            D3D11_SB_OPCODE_EMIT_STREAM : D3D11_SB_OPCODE_CUT_STREAM, src));
      return true;
   }

   case nir_intrinsic_terminate_if:
   case nir_intrinsic_demote_if:
   case nir_intrinsic_terminate:
   case nir_intrinsic_demote: {
      bool conditional = intr->intrinsic == nir_intrinsic_terminate_if || intr->intrinsic == nir_intrinsic_demote_if;
      COperandBase src = conditional ?
         nir_src_as_const_value_or_register(ctx, intr->src[0], 1, nullptr) :
         COperand(UINT_MAX);
      CInstruction inst(D3D10_SB_OPCODE_DISCARD, src);
      inst.m_Test = D3D10_SB_INSTRUCTION_TEST_NONZERO;
      ctx->mod.instructions.push_back(inst);
      return true;
   }

   default:
      fail(ctx, "unsupported intrinsic", &intr->instr);
      return false;
   }
}

static bool
emit_load_const(struct ntd_context *ctx, nir_load_const_instr *instr)
{
   COperand val;
   if (instr->def.num_components == 1)
      val.m_NumComponents = D3D10_SB_OPERAND_1_COMPONENT;
   else
      val.m_NumComponents = D3D10_SB_OPERAND_4_COMPONENT;

   if (instr->def.bit_size == 64) {
      val.m_Type = D3D10_SB_OPERAND_TYPE_IMMEDIATE64;
      if (instr->def.num_components > 2) {
         fail(ctx, "64-bit constant with more than 2 components", &instr->instr);
         return false;
      }
      memcpy(&val.m_Value64, &instr->value->i64, 8 * instr->def.num_components);
   } else if (instr->def.bit_size == 1) {
      val.m_Type = D3D10_SB_OPERAND_TYPE_IMMEDIATE32;
      for (unsigned i = 0; i < instr->def.num_components; ++i)
         val.m_Value[i] = instr->value[i].b ? UINT_MAX : 0;
   } else if (instr->def.bit_size == 32) {
      val.m_Type = D3D10_SB_OPERAND_TYPE_IMMEDIATE32;
      for (unsigned i = 0; i < instr->def.num_components; ++i)
         memcpy(&val.m_Value[i], &instr->value[i].u32, 4);
   } else {
      fail(ctx, "constant bit size", &instr->instr);
      return false;
   }

   ctx->mod.ssa_operands[instr->def.index].operand = val;
   return true;
}

static bool
emit_undef(struct ntd_context *ctx, nir_undef_instr *instr)
{
   COperand val;
   val.m_Type = D3D10_SB_OPERAND_TYPE_IMMEDIATE32;
   val.m_NumComponents = instr->def.num_components == 1 ? D3D10_SB_OPERAND_1_COMPONENT : D3D10_SB_OPERAND_4_COMPONENT;
   for (unsigned i = 0; i < 4; ++i)
      val.m_Value[i] = 0;
   ctx->mod.ssa_operands[instr->def.index].operand = val;
   return true;
}

static bool
emit_jump(struct ntd_context *ctx, nir_jump_instr *instr)
{
   switch (instr->type) {
   case nir_jump_break:
      ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_BREAK));
      return true;
   case nir_jump_continue:
      ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_CONTINUE));
      return true;
   default:
      fail(ctx, "unsupported jump", &instr->instr);
      return false;
   }
}

static bool
emit_deref(struct ntd_context* ctx, nir_deref_instr* instr)
{
   // In the GL environment samplers are already lowered to indices.
   if (!ctx->vulkan)
      return true;
   fail(ctx, "vulkan derefs not ported", &instr->instr);
   return false;
}

static bool
emit_tex(ntd_context *ctx, nir_tex_instr *instr)
{
   COperandBase tex;
   COperandBase sampler;
   COperandBase bias, lod = COperand(0u), min_lod;
   COperandBase coord, offset, dx, dy;
   COperandBase cmp, sample_idx;

   bool is_zero_lod = false;
   bool lod_needs_to_be_merged_to_coord_alpha = false;
   bool has_offset = false;

   if (is_sm51(ctx)) {
      nir_binding tex_binding = {};
      tex_binding.success = true;
      tex_binding.binding = instr->texture_index;
      nir_binding sampler_binding = {};
      sampler_binding.success = true;
      sampler_binding.binding = instr->sampler_index;
      nir_variable *tex_var = nir_get_binding_variable(ctx->shader, tex_binding);
      nir_variable *sampler_var = nir_get_binding_variable(ctx->shader, sampler_binding);
      if (tex_var)
         tex = COperand2D(D3D10_SB_OPERAND_TYPE_RESOURCE, tex_var->data.driver_location, instr->texture_index);
      else
         tex = COperand2D(D3D10_SB_OPERAND_TYPE_RESOURCE, instr->texture_index, instr->texture_index);
      if (sampler_var)
         sampler = COperand2D(D3D10_SB_OPERAND_TYPE_SAMPLER, sampler_var->data.driver_location, instr->sampler_index);
      else
         sampler = COperand2D(D3D10_SB_OPERAND_TYPE_SAMPLER, instr->sampler_index, instr->sampler_index);
   } else {
      tex = COperand(D3D10_SB_OPERAND_TYPE_RESOURCE, instr->texture_index);
      sampler = COperand(D3D10_SB_OPERAND_TYPE_SAMPLER, instr->sampler_index);
   }

   for (unsigned i = 0; i < instr->num_srcs; i++) {
      switch (instr->src[i].src_type) {
      case nir_tex_src_coord:
         coord = nir_src_as_const_value_or_register(ctx, instr->src[i].src, instr->coord_components, nullptr);
         break;
      case nir_tex_src_offset:
         has_offset = nir_src_is_const(instr->src[i].src);
         if (!has_offset) {
            fail(ctx, "non-constant texture offset", &instr->instr);
            return false;
         }
         offset = nir_src_as_const_value_or_register(ctx, instr->src[i].src, instr->coord_components, nullptr);
         break;
      case nir_tex_src_bias:
         bias = nir_src_as_const_value_or_register(ctx, instr->src[i].src, 1, nullptr);
         break;
      case nir_tex_src_lod:
         lod = nir_src_as_const_value_or_register(ctx, instr->src[i].src, 1, nullptr);
         lod_needs_to_be_merged_to_coord_alpha = instr->op == nir_texop_txf;
         is_zero_lod = nir_src_is_const(instr->src[i].src) && nir_src_as_uint(instr->src[i].src) == 0;
         break;
      case nir_tex_src_min_lod:
         min_lod = nir_src_as_const_value_or_register(ctx, instr->src[i].src, 1, nullptr);
         break;
      case nir_tex_src_comparator:
         cmp = nir_src_as_const_value_or_register(ctx, instr->src[i].src, 1, nullptr);
         break;
      case nir_tex_src_ddx:
         dx = nir_src_as_const_value_or_register(ctx, instr->src[i].src, instr->coord_components, nullptr);
         break;
      case nir_tex_src_ddy:
         dy = nir_src_as_const_value_or_register(ctx, instr->src[i].src, instr->coord_components, nullptr);
         break;
      case nir_tex_src_ms_index:
         sample_idx = nir_src_as_const_value_or_register(ctx, instr->src[i].src, 1, nullptr);
         break;
      case nir_tex_src_texture_offset:
      case nir_tex_src_sampler_offset:
         fail(ctx, "dynamic texture index", &instr->instr);
         return false;
      default:
         fail(ctx, "texture source", &instr->instr);
         return false;
      }
   }

   tex.m_NumComponents = D3D10_SB_OPERAND_4_COMPONENT;
   tex.SetSwizzle(0, 1, 2, 3);
   sampler.m_NumComponents = D3D10_SB_OPERAND_0_COMPONENT;

   if (lod_needs_to_be_merged_to_coord_alpha) {
      COperandDst temp_coords(D3D10_SB_OPERAND_TYPE_TEMP, ctx->mod.reg_alloc++);
      temp_coords.SetMask(0b111 << D3D10_SB_OPERAND_4_COMPONENT_MASK_SHIFT);
      ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_MOV, temp_coords, coord));
      temp_coords.SetMask(D3D10_SB_OPERAND_4_COMPONENT_MASK_W);
      ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_MOV, temp_coords, lod));
      coord = COperand4(D3D10_SB_OPERAND_TYPE_TEMP, temp_coords.RegIndex(0));
   }

   COperandBase dest = def_as_register(ctx, &instr->def, 0b1111);
   CInstruction inst;
   switch (instr->op) {
   case nir_texop_txb:
      inst = CInstruction(D3D10_SB_OPCODE_SAMPLE_B, dest, coord, tex, sampler, bias);
      break;
   case nir_texop_tex:
      if (instr->is_shadow) {
         D3D10_SB_OPCODE_TYPE opcode = ctx->mod.shader_kind != D3D10_SB_PIXEL_SHADER || is_zero_lod ?
            D3D10_SB_OPCODE_SAMPLE_C_LZ : D3D10_SB_OPCODE_SAMPLE_C;
         inst = CInstruction(opcode, dest, coord, tex, sampler, cmp);
         break;
      } else if (ctx->mod.shader_kind == D3D10_SB_PIXEL_SHADER) {
         inst = CInstruction(D3D10_SB_OPCODE_SAMPLE, dest, coord, tex, sampler);
         break;
      }
      lod = COperand(0u);
      FALLTHROUGH;
   case nir_texop_txl:
      if (instr->is_shadow)
         inst = CInstruction(D3D10_SB_OPCODE_SAMPLE_C_LZ, dest, coord, tex, sampler, cmp);
      else
         inst = CInstruction(D3D10_SB_OPCODE_SAMPLE_L, dest, coord, tex, sampler, lod);
      break;
   case nir_texop_txd:
      inst = CInstruction(D3D10_SB_OPCODE_SAMPLE_D, dest, coord, tex, sampler, dx, dy);
      break;
   case nir_texop_txf:
      inst = CInstruction(D3D10_SB_OPCODE_LD, dest, coord, tex);
      break;
   case nir_texop_txf_ms:
      inst = CInstruction(D3D10_SB_OPCODE_LD_MS, dest, coord, tex, sample_idx);
      break;
   case nir_texop_txs:
      if (instr->sampler_dim == GLSL_SAMPLER_DIM_BUF)
         inst = CInstruction(D3D11_SB_OPCODE_BUFINFO, dest, tex);
      else {
         inst = CInstruction(D3D10_SB_OPCODE_RESINFO, dest, lod, tex);
         inst.m_ResInfoReturnType = D3D10_SB_RESINFO_INSTRUCTION_RETURN_UINT;
      }
      break;
   case nir_texop_lod:
      inst = CInstruction(D3D10_1_SB_OPCODE_LOD, dest, coord, tex, sampler);
      break;
   case nir_texop_query_levels:
      tex.SetSwizzle(3, 3, 3, 3);
      lod = COperand(0u);
      inst = CInstruction(D3D10_SB_OPCODE_RESINFO, dest, lod, tex);
      inst.m_ResInfoReturnType = D3D10_SB_RESINFO_INSTRUCTION_RETURN_UINT;
      break;
   default:
      fail(ctx, "texture op", &instr->instr);
      return false;
   }

   if (has_offset) {
      int *offsets = reinterpret_cast<int*>(offset.m_Value);
      inst.SetTexelOffset(offsets[0], offsets[1], offsets[2]);
   }

   store_instruction(ctx, &instr->def, inst);
   return true;
}

static bool
emit_instr(struct ntd_context *ctx, struct nir_instr *instr)
{
   switch (instr->type) {
   case nir_instr_type_alu:
      return emit_alu(ctx, nir_instr_as_alu(instr));
   case nir_instr_type_intrinsic:
      return emit_intrinsic(ctx, nir_instr_as_intrinsic(instr));
   case nir_instr_type_load_const:
      return emit_load_const(ctx, nir_instr_as_load_const(instr));
   case nir_instr_type_deref:
      return emit_deref(ctx, nir_instr_as_deref(instr));
   case nir_instr_type_jump:
      return emit_jump(ctx, nir_instr_as_jump(instr));
   case nir_instr_type_tex:
      return emit_tex(ctx, nir_instr_as_tex(instr));
   case nir_instr_type_undef:
      return emit_undef(ctx, nir_instr_as_undef(instr));
   default:
      fail(ctx, "unsupported instruction type", instr);
      return false;
   }
}

static bool
emit_block(struct ntd_context *ctx, struct nir_block *block)
{
   nir_foreach_instr(instr, block)
   {
      TRACE_CONVERSION(instr);
      if (!emit_instr(ctx, instr))
         return false;
   }
   return true;
}

static bool
emit_cf_list(struct ntd_context *ctx, struct exec_list *list);

static bool
emit_if(struct ntd_context *ctx, struct nir_if *if_stmt)
{
   COperandBase cond = nir_src_as_const_value_or_register(ctx, if_stmt->condition, 1, nullptr);
   ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_IF, cond, D3D10_SB_INSTRUCTION_TEST_NONZERO));

   if (!emit_cf_list(ctx, &if_stmt->then_list))
      return false;

   if (!exec_list_is_empty(&if_stmt->else_list)) {
      ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_ELSE));
      if (!emit_cf_list(ctx, &if_stmt->else_list))
         return false;
   }

   ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_ENDIF));
   return true;
}

static bool
emit_loop(struct ntd_context *ctx, nir_loop *loop)
{
   if (nir_loop_has_continue_construct(loop)) {
      fail(ctx, "loop with continue construct", NULL);
      return false;
   }
   ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_LOOP));
   if (!emit_cf_list(ctx, &loop->body))
      return false;
   ctx->mod.instructions.push_back(CInstruction(D3D10_SB_OPCODE_ENDLOOP));
   return true;
}

static bool
emit_cf_list(struct ntd_context *ctx, struct exec_list *list)
{
   foreach_list_typed(nir_cf_node, node, node, list)
   {
      switch (node->type) {
      case nir_cf_node_block:
         if (!emit_block(ctx, nir_cf_node_as_block(node)))
            return false;
         break;
      case nir_cf_node_if:
         if (!emit_if(ctx, nir_cf_node_as_if(node)))
            return false;
         break;
      case nir_cf_node_loop:
         if (!emit_loop(ctx, nir_cf_node_as_loop(node)))
            return false;
         break;
      default:
         fail(ctx, "unsupported cf node", NULL);
         return false;
      }
   }
   return true;
}

static unsigned get_dword_size(const struct glsl_type *type)
{
   return glsl_count_attribute_slots(type, false);
}

static D3D10_SB_PRIMITIVE
dxbc_get_input_primitive(enum mesa_prim primitive)
{
   switch (primitive) {
   case MESA_PRIM_POINTS: return D3D10_SB_PRIMITIVE_POINT;
   case MESA_PRIM_LINES: return D3D10_SB_PRIMITIVE_LINE;
   case MESA_PRIM_LINES_ADJACENCY: return D3D10_SB_PRIMITIVE_LINE_ADJ;
   case MESA_PRIM_TRIANGLES: return D3D10_SB_PRIMITIVE_TRIANGLE;
   case MESA_PRIM_TRIANGLES_ADJACENCY: return D3D10_SB_PRIMITIVE_TRIANGLE_ADJ;
   default: return D3D10_SB_PRIMITIVE_UNDEFINED;
   }
}

static D3D10_SB_PRIMITIVE_TOPOLOGY
dxbc_get_primitive_topology(enum mesa_prim topology)
{
   switch (topology) {
   case MESA_PRIM_POINTS: return D3D10_SB_PRIMITIVE_TOPOLOGY_POINTLIST;
   case MESA_PRIM_LINES: return D3D10_SB_PRIMITIVE_TOPOLOGY_LINELIST;
   case MESA_PRIM_LINE_STRIP: return D3D10_SB_PRIMITIVE_TOPOLOGY_LINESTRIP;
   case MESA_PRIM_TRIANGLES: return D3D10_SB_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
   case MESA_PRIM_TRIANGLE_STRIP: return D3D10_SB_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
   default: return D3D10_SB_PRIMITIVE_TOPOLOGY_UNDEFINED;
   }
}

static D3D10_SB_INTERPOLATION_MODE
dxil_to_dxbc_interpolation_mode(dxil_interpolation_mode mode)
{
   switch (mode) {
   case DXIL_INTERP_CONSTANT: return D3D10_SB_INTERPOLATION_CONSTANT;
   case DXIL_INTERP_LINEAR: return D3D10_SB_INTERPOLATION_LINEAR;
   case DXIL_INTERP_LINEAR_CENTROID: return D3D10_SB_INTERPOLATION_LINEAR_CENTROID;
   case DXIL_INTERP_LINEAR_NOPERSPECTIVE: return D3D10_SB_INTERPOLATION_LINEAR_NOPERSPECTIVE;
   case DXIL_INTERP_LINEAR_NOPERSPECTIVE_CENTROID: return D3D10_SB_INTERPOLATION_LINEAR_NOPERSPECTIVE_CENTROID;
   case DXIL_INTERP_LINEAR_NOPERSPECTIVE_SAMPLE: return D3D10_SB_INTERPOLATION_LINEAR_NOPERSPECTIVE_SAMPLE;
   case DXIL_INTERP_LINEAR_SAMPLE: return D3D10_SB_INTERPOLATION_LINEAR_SAMPLE;
   default: return D3D10_SB_INTERPOLATION_UNDEFINED;
   }
}

static void
count_resources(struct ntd_context *ctx)
{
   if (is_sm51(ctx)) {
      unsigned num_ubos = 0;
      nir_foreach_variable_with_modes(ubo, ctx->shader, nir_var_mem_ubo) {
         ubo->data.driver_location = num_ubos++;
      }

      unsigned num_samplers = 0;
      nir_foreach_variable_with_modes(var, ctx->shader, nir_var_uniform) {
         unsigned count = glsl_type_get_sampler_count(var->type);
         if (count > 0 && glsl_type_is_bare_sampler(glsl_without_array(var->type)))
            var->data.driver_location = num_samplers++;
      }

      unsigned num_textures = 0;
      nir_foreach_variable_with_modes(var, ctx->shader, nir_var_uniform) {
         unsigned count = glsl_type_get_texture_count(var->type);
         if (count > 0)
            var->data.driver_location = num_textures++;
      }
   }

   nir_function_impl *entrypoint = nir_shader_get_entrypoint(ctx->shader);
   ctx->mod.ssa_operands.reset(new ntd_ssa[entrypoint->ssa_alloc]);
}

static D3D10_SB_RESOURCE_DIMENSION
glsl_sampler_dim_to_dxbc(glsl_sampler_dim dim, bool is_array)
{
   switch (dim) {
   case GLSL_SAMPLER_DIM_1D:
      return is_array ? D3D10_SB_RESOURCE_DIMENSION_TEXTURE1DARRAY : D3D10_SB_RESOURCE_DIMENSION_TEXTURE1D;
   case GLSL_SAMPLER_DIM_2D:
   case GLSL_SAMPLER_DIM_EXTERNAL:
   case GLSL_SAMPLER_DIM_RECT:
      return is_array ? D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DARRAY : D3D10_SB_RESOURCE_DIMENSION_TEXTURE2D;
   case GLSL_SAMPLER_DIM_3D:
      return D3D10_SB_RESOURCE_DIMENSION_TEXTURE3D;
   case GLSL_SAMPLER_DIM_CUBE:
      return is_array ? D3D10_SB_RESOURCE_DIMENSION_TEXTURECUBEARRAY : D3D10_SB_RESOURCE_DIMENSION_TEXTURECUBE;
   case GLSL_SAMPLER_DIM_BUF:
      return D3D10_SB_RESOURCE_DIMENSION_BUFFER;
   case GLSL_SAMPLER_DIM_MS:
      return is_array ? D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DMSARRAY : D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DMS;
   default:
      return D3D10_SB_RESOURCE_DIMENSION_UNKNOWN;
   }
}

static D3D10_SB_RESOURCE_RETURN_TYPE
glsl_base_type_to_dxbc_ret_type(glsl_base_type type)
{
   switch (type) {
   case GLSL_TYPE_UINT: return D3D10_SB_RETURN_TYPE_UINT;
   case GLSL_TYPE_INT: return D3D10_SB_RETURN_TYPE_SINT;
   default:
   case GLSL_TYPE_FLOAT: return D3D10_SB_RETURN_TYPE_FLOAT;
   }
}

static bool
emit_dcl(struct ntd_context *ctx)
{
   ctx->mod.shader.EmitGlobalFlagsDecl(D3D10_SB_GLOBAL_FLAG_REFACTORING_ALLOWED);

   nir_foreach_variable_with_modes(ubo, ctx->shader, nir_var_mem_ubo) {
      unsigned size = get_dword_size(ubo->interface_type ? ubo->interface_type : ubo->type);
      if (is_sm51(ctx)) {
         ctx->mod.shader.EmitIndexableConstantBufferDecl(ubo->data.driver_location,
            ubo->data.binding, ubo->data.binding, size,
            D3D10_SB_CONSTANT_BUFFER_DYNAMIC_INDEXED, ubo->data.descriptor_set);
      } else {
         ctx->mod.shader.EmitConstantBufferDecl(ubo->data.binding, size, D3D10_SB_CONSTANT_BUFFER_DYNAMIC_INDEXED);
      }
   }

   nir_foreach_variable_with_modes(var, ctx->shader, nir_var_uniform) {
      unsigned count = glsl_type_get_sampler_count(var->type);
      if (count > 0 && glsl_type_is_bare_sampler(glsl_without_array(var->type))) {
         D3D10_SB_SAMPLER_MODE mode = glsl_sampler_type_is_shadow(glsl_without_array(var->type)) ?
            D3D10_SB_SAMPLER_MODE_COMPARISON : D3D10_SB_SAMPLER_MODE_DEFAULT;
         if (is_sm51(ctx)) {
            ctx->mod.shader.EmitIndexableSamplerDecl(var->data.driver_location,
               var->data.binding, var->data.binding + count - 1, mode, var->data.descriptor_set);
         } else {
            for (unsigned i = 0; i < count; ++i)
               ctx->mod.shader.EmitSamplerDecl(var->data.binding + i, mode);
         }
      }
   }

   nir_foreach_variable_with_modes(var, ctx->shader, nir_var_uniform) {
      unsigned count = glsl_type_get_texture_count(var->type);
      if (count > 0) {
         const glsl_type *type = glsl_without_array(var->type);
         D3D10_SB_RESOURCE_DIMENSION dim = glsl_sampler_dim_to_dxbc(glsl_get_sampler_dim(type), glsl_sampler_type_is_array(type));
         D3D10_SB_RESOURCE_RETURN_TYPE ret_type = glsl_base_type_to_dxbc_ret_type(glsl_get_sampler_result_type(type));
         if (is_sm51(ctx)) {
            ctx->mod.shader.EmitIndexableResourceDecl(var->data.driver_location,
               var->data.binding, var->data.binding + count - 1,
               dim, ret_type, ret_type, ret_type, ret_type, var->data.descriptor_set);
         } else {
            for (unsigned i = 0; i < count; ++i)
               ctx->mod.shader.EmitResourceDecl(dim, var->data.binding + i, ret_type, ret_type, ret_type, ret_type);
         }
      }
   }

   for (unsigned i = 0; i < ctx->dxil_mod.num_sig_inputs; i++) {
      struct dxil_signature_record &input = ctx->dxil_mod.inputs[i];
      struct dxil_psv_signature_element &psv_input = ctx->dxil_mod.psv_inputs[i];
      for (unsigned e = 0; e < input.num_elements; e++) {
         struct dxil_signature_element &elem = input.elements[e];
         UINT write_mask = elem.mask << D3D10_SB_OPERAND_4_COMPONENT_MASK_SHIFT;
         enum dxil_prog_sig_semantic sem = static_cast<enum dxil_prog_sig_semantic>(elem.system_value);
         bool gs = ctx->mod.shader_kind == D3D10_SB_GEOMETRY_SHADER;
         unsigned vertices = gs ? mesa_vertices_per_prim(ctx->shader->info.gs.input_primitive) : 0;
         switch (sem) {
         case DXIL_PROG_SEM_UNDEFINED:
         case DXIL_PROG_SEM_TARGET:
            if (ctx->mod.shader_kind == D3D10_SB_PIXEL_SHADER) {
               ctx->mod.shader.EmitPSInputDecl(elem.reg, write_mask,
                  dxil_to_dxbc_interpolation_mode((dxil_interpolation_mode)psv_input.interpolation_mode));
            } else if (gs) {
               ctx->mod.shader.EmitGSInputDecl(elem.reg, vertices, write_mask);
            } else {
               ctx->mod.shader.EmitInputDecl(D3D10_SB_OPERAND_TYPE_INPUT, elem.reg, write_mask);
            }
            break;
         case DXIL_PROG_SEM_POSITION:
            if (ctx->mod.shader_kind == D3D10_SB_PIXEL_SHADER) {
               ctx->mod.shader.EmitPSInputSystemInterpretedValueDecl(elem.reg, write_mask,
                  D3D10_SB_INTERPOLATION_LINEAR_NOPERSPECTIVE, D3D10_SB_NAME_POSITION);
            } else if (gs) {
               ctx->mod.shader.EmitGSInputSystemInterpretedValueDecl(elem.reg, vertices, write_mask, D3D10_SB_NAME_POSITION);
            } else {
               ctx->mod.shader.EmitInputSystemInterpretedValueDecl(D3D10_SB_OPERAND_TYPE_INPUT, elem.reg, write_mask, D3D10_SB_NAME_POSITION);
            }
            break;
         case DXIL_PROG_SEM_PRIMITIVE_ID:
            ctx->mod.shader.EmitInputPrimIdDecl();
            break;
         case DXIL_PROG_SEM_VERTEX_ID:
         case DXIL_PROG_SEM_INSTANCE_ID:
         case DXIL_PROG_SEM_IS_FRONTFACE:
         case DXIL_PROG_SEM_SAMPLE_INDEX:
            ctx->mod.shader.EmitInputSystemGeneratedValueDecl(elem.reg, write_mask, static_cast<D3D10_SB_NAME>(sem));
            break;
         default:
            fail(ctx, "unhandled input semantic", NULL);
            return false;
         }
      }
   }

   uint32_t num_temps = ctx->mod.reg_alloc;
   if (num_temps > 0)
      ctx->mod.shader.EmitTempsDecl(num_temps);

   if (ctx->mod.shader_kind == D3D10_SB_GEOMETRY_SHADER) {
      ctx->mod.shader.EmitGSInputPrimitiveDecl(dxbc_get_input_primitive(ctx->shader->info.gs.input_primitive));
      for (unsigned i = 0; i < 4; ++i) {
         if (ctx->shader->info.gs.active_stream_mask & (1 << i))
            ctx->mod.shader.EmitStreamDecl(i);
      }
      ctx->mod.shader.EmitGSOutputTopologyDecl(dxbc_get_primitive_topology(ctx->shader->info.gs.output_primitive));
   }

   for (unsigned i = 0; i < ctx->dxil_mod.num_sig_outputs; i++) {
      struct dxil_signature_record &output = ctx->dxil_mod.outputs[i];
      for (unsigned e = 0; e < output.num_elements; e++) {
         struct dxil_signature_element &elem = output.elements[e];
         UINT write_mask = elem.mask << D3D10_SB_OPERAND_4_COMPONENT_MASK_SHIFT;
         enum dxil_prog_sig_semantic sem = static_cast<enum dxil_prog_sig_semantic>(elem.system_value);
         switch (sem) {
         case DXIL_PROG_SEM_UNDEFINED:
         case DXIL_PROG_SEM_TARGET:
            ctx->mod.shader.EmitOutputDecl(elem.reg, write_mask);
            break;
         case DXIL_PROG_SEM_POSITION:
            ctx->mod.shader.EmitOutputSystemInterpretedValueDecl(elem.reg, write_mask, D3D10_SB_NAME_POSITION);
            break;
         case DXIL_PROG_SEM_DEPTH:
            ctx->mod.shader.EmitODepthDecl();
            break;
         case DXIL_PROG_SEM_DEPTH_GE:
            ctx->mod.shader.EmitODepthDeclGE();
            break;
         case DXIL_PROG_SEM_DEPTH_LE:
            ctx->mod.shader.EmitODepthDeclLE();
            break;
         case DXIL_PROG_SEM_STENCIL_REF:
            ctx->mod.shader.EmitOStencilRefDecl();
            break;
         default:
            fail(ctx, "unhandled output semantic", NULL);
            return false;
         }
      }
   }

   if (ctx->mod.shader_kind == D3D10_SB_GEOMETRY_SHADER)
      ctx->mod.shader.EmitGSMaxOutputVertexCountDecl(ctx->shader->info.gs.vertices_out);

   if (ctx->mod.shader_kind == D3D11_SB_COMPUTE_SHADER) {
      const unsigned mask_3comp = 0b111 << D3D10_SB_OPERAND_4_COMPONENT_MASK_SHIFT;
      if (BITSET_TEST(ctx->shader->info.system_values_read, SYSTEM_VALUE_GLOBAL_INVOCATION_ID))
         ctx->mod.shader.EmitInputThreadIDDecl(mask_3comp);
      if (BITSET_TEST(ctx->shader->info.system_values_read, SYSTEM_VALUE_WORKGROUP_ID))
         ctx->mod.shader.EmitInputThreadGroupIDDecl(mask_3comp);
      if (BITSET_TEST(ctx->shader->info.system_values_read, SYSTEM_VALUE_LOCAL_INVOCATION_ID))
         ctx->mod.shader.EmitInputThreadIDInGroupDecl(mask_3comp);
      if (BITSET_TEST(ctx->shader->info.system_values_read, SYSTEM_VALUE_LOCAL_INVOCATION_INDEX))
         ctx->mod.shader.EmitInputThreadIDInGroupFlattenedDecl();
      if (ctx->shader->info.shared_size)
         ctx->mod.shader.EmitRawThreadGroupSharedMemoryDecl(0, ctx->shader->info.shared_size);
      ctx->mod.shader.EmitThreadGroupDecl(ctx->shader->info.workgroup_size[0],
                                          ctx->shader->info.workgroup_size[1],
                                          ctx->shader->info.workgroup_size[2]);
   }

   return true;
}

static bool
emit_module(struct ntd_context *ctx)
{
   nir_function_impl *entry = nir_shader_get_entrypoint(ctx->shader);
   nir_metadata_require(entry, nir_metadata_block_index);

   count_resources(ctx);

   if (!emit_cf_list(ctx, &entry->body))
      return false;

   if (!emit_dcl(ctx))
      return false;

   for (auto& instr : ctx->mod.instructions)
      ctx->mod.shader.EmitInstruction(instr);

   ctx->mod.shader.EmitInstruction(CInstruction(D3D10_SB_OPCODE_RET));
   return true;
}

static D3D10_SB_TOKENIZED_PROGRAM_TYPE
get_dxbc_shader_kind(struct nir_shader *s)
{
   switch (s->info.stage) {
   case MESA_SHADER_VERTEX: return D3D10_SB_VERTEX_SHADER;
   case MESA_SHADER_GEOMETRY: return D3D10_SB_GEOMETRY_SHADER;
   case MESA_SHADER_FRAGMENT: return D3D10_SB_PIXEL_SHADER;
   case MESA_SHADER_KERNEL:
   case MESA_SHADER_COMPUTE: return D3D11_SB_COMPUTE_SHADER;
   default: return D3D11_SB_COMPUTE_SHADER;
   }
}

static enum dxil_shader_kind
get_dxil_shader_kind(struct nir_shader *s)
{
   switch (s->info.stage) {
   case MESA_SHADER_VERTEX: return DXIL_VERTEX_SHADER;
   case MESA_SHADER_GEOMETRY: return DXIL_GEOMETRY_SHADER;
   case MESA_SHADER_FRAGMENT: return DXIL_PIXEL_SHADER;
   default: return DXIL_COMPUTE_SHADER;
   }
}

static unsigned
type_size_vec4(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

static bool
lower_bit_size_callback_wrap(const nir_instr *instr, void *data)
{
   (void)data;
   if (instr->type != nir_instr_type_alu)
      return 0;
   const nir_alu_instr *alu = nir_instr_as_alu(instr);
   unsigned bits = alu->def.bit_size;
   if (bits == 1)
      return 0;
   if (bits < 32)
      return 32;
   return 0;
}

static void
optimize_nir(struct nir_shader *s)
{
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, s, nir_lower_vars_to_ssa);
      NIR_PASS(progress, s, nir_lower_indirect_derefs_to_if_else_trees, nir_var_function_temp, 4);
      NIR_PASS(progress, s, nir_opt_copy_prop);
      NIR_PASS(progress, s, nir_opt_copy_prop_vars);
      NIR_PASS(progress, s, nir_lower_bit_size, (nir_lower_bit_size_callback)lower_bit_size_callback_wrap, NULL);
      NIR_PASS(progress, s, nir_opt_remove_phis);
      NIR_PASS(progress, s, nir_opt_dce);
      NIR_PASS(progress, s, nir_opt_if, nir_opt_if_optimize_phi_true_false | nir_opt_if_avoid_64bit_phis);
      NIR_PASS(progress, s, nir_opt_dead_cf);
      NIR_PASS(progress, s, nir_opt_cse);
      nir_opt_peephole_select_options peephole_select_options = {};
      peephole_select_options.limit = 8;
      peephole_select_options.indirect_load_ok = true;
      peephole_select_options.expensive_alu_ok = true;
      NIR_PASS(progress, s, nir_opt_peephole_select, &peephole_select_options);
      NIR_PASS(progress, s, nir_opt_algebraic);
      NIR_PASS(progress, s, dxil_nir_algebraic);
      NIR_PASS(progress, s, nir_lower_alu);
      NIR_PASS(progress, s, nir_opt_constant_folding);
      NIR_PASS(progress, s, nir_opt_undef);
      NIR_PASS(progress, s, nir_opt_deref);
      NIR_PASS(progress, s, nir_lower_64bit_phis);
      NIR_PASS(progress, s, nir_opt_loop_unroll);
      NIR_PASS(progress, s, nir_lower_pack);
      NIR_PASS(progress, s, dxil_nir_remove_oob_array_accesses);
      NIR_PASS(progress, s, nir_lower_system_values);
   } while (progress);

   do {
      progress = false;
      NIR_PASS(progress, s, nir_opt_algebraic_late);
   } while (progress);

   NIR_PASS(_, s, nir_lower_undef_to_zero, NULL);
}

/* Mirrors allocate_sysvalues in nir_to_dxil.c */
static nir_variable *
add_sysvalue(struct ntd_context *ctx, uint8_t value, const char *name, int driver_location)
{
   nir_variable *var = rzalloc(ctx->shader, nir_variable);
   if (!var)
      return NULL;
   var->data.driver_location = driver_location;
   var->data.location = value;
   var->type = glsl_uint_type();
   var->name = ralloc_strdup(var, name);
   var->data.mode = nir_var_system_value;
   var->data.interpolation = INTERP_MODE_FLAT;
   return var;
}

static bool
append_input_or_sysvalue(struct ntd_context *ctx, int input_loc, int sv_slot, const char *name, int driver_location)
{
   if (input_loc >= 0) {
      nir_foreach_variable_with_modes(var, ctx->shader, nir_var_shader_in) {
         if (var->data.location == input_loc) {
            ctx->system_value[sv_slot] = var;
            return true;
         }
      }
   }
   ctx->system_value[sv_slot] = add_sysvalue(ctx, sv_slot, name, driver_location);
   if (!ctx->system_value[sv_slot])
      return false;
   nir_shader_add_variable(ctx->shader, ctx->system_value[sv_slot]);
   return true;
}

struct sysvalue_name {
   gl_system_value value;
   int slot;
   const char *name;
   mesa_shader_stage only_in_shader;
};

static const struct sysvalue_name possible_sysvalues[] = {
   {SYSTEM_VALUE_VERTEX_ID_ZERO_BASE, -1, "SV_VertexID", MESA_SHADER_NONE},
   {SYSTEM_VALUE_INSTANCE_ID, -1, "SV_InstanceID", MESA_SHADER_NONE},
   {SYSTEM_VALUE_FRONT_FACE, VARYING_SLOT_FACE, "SV_IsFrontFace", MESA_SHADER_NONE},
   {SYSTEM_VALUE_PRIMITIVE_ID, VARYING_SLOT_PRIMITIVE_ID, "SV_PrimitiveID", MESA_SHADER_GEOMETRY},
   {SYSTEM_VALUE_SAMPLE_ID, -1, "SV_SampleIndex", MESA_SHADER_NONE},
};

static bool
allocate_sysvalues(struct ntd_context *ctx)
{
   unsigned driver_location = 0;
   nir_foreach_variable_with_modes(var, ctx->shader, nir_var_shader_in)
      driver_location = MAX2(driver_location, var->data.driver_location + 1);
   nir_foreach_variable_with_modes(var, ctx->shader, nir_var_system_value)
      driver_location = MAX2(driver_location, var->data.driver_location + 1);

   for (unsigned i = 0; i < ARRAY_SIZE(possible_sysvalues); ++i) {
      const struct sysvalue_name *info = &possible_sysvalues[i];
      if (info->only_in_shader != MESA_SHADER_NONE && info->only_in_shader != ctx->shader->info.stage)
         continue;
      if (BITSET_TEST(ctx->shader->info.system_values_read, info->value)) {
         if (!append_input_or_sysvalue(ctx, info->slot, info->value, info->name, driver_location++))
            return false;
      }
   }
   return true;
}

bool
nir_to_dxbc(struct nir_shader *s, const struct nir_to_dxil_options *opts,
            struct blob *blob, char *err, size_t errlen)
{
   assert(opts);
   debug_dxbc = (int)debug_get_option_debug_dxbc();
   if (err && errlen)
      err[0] = 0;

   if (opts->shader_model_max < SHADER_MODEL_5_0) {
      snprintf(err, errlen, "invalid shader model requested");
      return false;
   }

   ntd_context ctx;
   ctx.shader = s;
   ctx.opts = opts;
   ctx.err = err;
   ctx.errlen = errlen;
   ctx.vulkan = opts->environment == DXIL_ENVIRONMENT_VULKAN;
   ctx.mod.shader_kind = get_dxbc_shader_kind(s);
   ctx.mod.major_version = 5;
   ctx.mod.minor_version = opts->shader_model_max >= SHADER_MODEL_5_1 ? 1 : 0;
   ctx.dxil_mod.shader_kind = get_dxil_shader_kind(s);
   ctx.dxil_mod.major_version = 6;
   ctx.dxil_mod.minor_version = 0;

   if (s->info.stage == MESA_SHADER_TESS_CTRL || s->info.stage == MESA_SHADER_TESS_EVAL) {
      snprintf(err, errlen, "tessellation not supported by nir_to_dxbc");
      return false;
   }

   NIR_PASS(_, s, nir_lower_pack);
   NIR_PASS(_, s, nir_lower_frexp);
   NIR_PASS(_, s, nir_lower_flrp, 16 | 32 | 64, true);
   NIR_PASS(_, s, nir_lower_io, (nir_variable_mode)(nir_var_shader_in | nir_var_shader_out), type_size_vec4, nir_lower_io_lower_64bit_to_32);
   s->info.disable_input_offset_src_constant_folding = true;
   s->info.disable_output_offset_src_constant_folding = true;
   NIR_PASS(_, s, dxil_nir_ensure_position_writes);
   NIR_PASS(_, s, dxil_nir_lower_system_values);

   optimize_nir(s);

   nir_load_store_vectorize_options vectorize_opts = {};
   vectorize_opts.modes = nir_var_mem_ubo;
   NIR_PASS(_, s, nir_opt_load_store_vectorize, &vectorize_opts);
   NIR_PASS(_, s, nir_lower_ubo_vec4);

   optimize_nir(s);

   NIR_PASS(_, s, nir_remove_dead_variables, (nir_variable_mode)(nir_var_function_temp | nir_var_mem_constant | nir_var_mem_shared), NULL);

   if (!allocate_sysvalues(&ctx)) {
      snprintf(err, errlen, "allocate_sysvalues failed");
      return false;
   }
   NIR_PASS(_, s, dxil_nir_lower_sysval_to_load_input, ctx.system_value);
   NIR_PASS(_, s, nir_opt_dce);

   NIR_PASS(_, s, nir_lower_bool_to_int32);
   NIR_PASS(_, s, nir_opt_dce);
   NIR_PASS(_, s, nir_convert_from_ssa, true, false);

   nir_function_impl *entrypoint = nir_shader_get_entrypoint(s);
   nir_index_ssa_defs(entrypoint);

   if (debug_dxbc & DXBC_DEBUG_VERBOSE)
      nir_print_shader(s, stderr);

   preprocess_signatures(&ctx.dxil_mod, s, opts->input_clip_size);

   DxbcModule &mod = ctx.mod;
   mod.shader.Init(1024);
   mod.shader.StartShader(mod.shader_kind, mod.major_version, mod.minor_version);
   mod.reg_alloc = 0;

   if (!emit_module(&ctx)) {
      if (err && !err[0])
         snprintf(err, errlen, "emit_module failed");
      return false;
   }

   mod.shader.EndShader();

   ScopedDxilContainer container;

   if (!dxil_container_add_io_signature(container.get(), DXIL_ISG1,
                                        ctx.dxil_mod.num_sig_inputs,
                                        ctx.dxil_mod.inputs, false)) {
      snprintf(err, errlen, "dxil_container_add_io_signature (inputs) failed");
      return false;
   }

   if (!dxil_container_add_io_signature(container.get(), DXIL_OSG1,
                                        ctx.dxil_mod.num_sig_outputs,
                                        ctx.dxil_mod.outputs, false)) {
      snprintf(err, errlen, "dxil_container_add_io_signature (outputs) failed");
      return false;
   }

   if (!dxil_container_add_shader_blob(
           container.get(), static_cast<const void *>(mod.shader.GetShader()),
           static_cast<uint32_t>(mod.shader.ShaderSizeInDWORDs() * sizeof(UINT)))) {
      snprintf(err, errlen, "dxil_container_add_shader_blob failed");
      return false;
   }

   blob_init(blob);
   if (!dxil_container_write(container.get(), blob)) {
      snprintf(err, errlen, "dxil_container_write failed");
      return false;
   }

   return true;
}
