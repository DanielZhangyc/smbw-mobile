using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System.Collections.Generic;

namespace Ryujinx.Graphics.Shader.Translation
{
    // 只读 SSA 证明，不改着色器；未知源、phi、运算或资源数组均保守保留完整访问域。
    // Read-only SSA proof, without changing shaders; unknown sources, phis, arithmetic and descriptor arrays keep the full access domain.
    public record TextureSampleValue(bool IsConstant, bool IsFloat, int Value, int Slot, int ByteOffset, bool IntegerU16);
    public record TextureSampleRead(TextureSampleValue Lod, TextureSampleValue Layer);

    public class TextureSampleAccess
    {
        public int Set { get; init; }
        public int Binding { get; init; }
        public string Name { get; init; }
        public bool Complete { get; internal set; } = true;
        public List<TextureSampleRead> Reads { get; } = new();

        internal static TextureSampleAccess[] Collect(IReadOnlyList<Function> functions, ResourceManager resources)
        {
            TextureDescriptor[] descriptors = resources.GetTextureDescriptors();
            TextureSampleAccess[] result = new TextureSampleAccess[descriptors.Length];
            for (int i = 0; i < result.Length; i++)
            {
                result[i] = new TextureSampleAccess { Set = descriptors[i].Set, Binding = descriptors[i].Binding,
                    Name = resources.Properties.Textures[new SetBindingPair(descriptors[i].Set, descriptors[i].Binding)].Name };
            }

            foreach (Function function in functions)
            foreach (BasicBlock block in function.Blocks)
            foreach (INode node in block.Operations)
            {
                if (node is not TextureOperation texture || texture.Inst.IsImage()) continue;
                int index = System.Array.FindIndex(descriptors, d => d.Set == texture.Set && d.Binding == texture.Binding);
                if (index < 0)
                {
                    // 未解析的无绑定资源可能指向任一已登记贴图，不能对其中某张声称访问已完整覆盖。
                    // An unresolved bindless resource may refer to any registered texture; do not claim complete coverage for one of them.
                    foreach (TextureSampleAccess access in result) access.Complete = false;
                    continue;
                }
                TextureSampleAccess current = result[index];
                if (texture.Inst.IsTextureQuery()) continue;
                if (texture.Inst != Instruction.TextureSample || descriptors[index].ArrayLength != 1 ||
                    (texture.Flags & TextureFlags.Bindless) != 0 ||
                    (texture.SamplerBinding >= 0 && resources.IsArrayOfTexturesOrImages(texture.SamplerBinding, false)))
                {
                    current.Complete = false;
                    continue;
                }

                int dimensions = texture.Type.Dimensions;
                bool array = (texture.Type & SamplerType.Array) != 0;
                bool cube = (texture.Type & SamplerType.Mask) == SamplerType.TextureCube;
                TextureSampleValue layer = array && cube ? LayerValue(texture.GetSource(dimensions), 0) : null;
                int lodIndex = dimensions + (array ? 1 : 0) + ((texture.Type & SamplerType.Shadow) != 0 ? 1 : 0);
                if ((texture.Flags & TextureFlags.Derivatives) != 0) lodIndex += dimensions * 2;
                TextureSampleValue lod = null;
                if ((texture.Flags & TextureFlags.LodLevel) != 0 &&
                    (texture.Flags & (TextureFlags.IntCoords | TextureFlags.Gather | TextureFlags.Derivatives | TextureFlags.LodBias)) == 0 &&
                    (texture.Type & SamplerType.Multisample) == 0)
                {
                    lod = DirectValue(texture.GetSource(lodIndex), true, false, 0);
                }
                TextureSampleRead read = new(lod, layer);
                if (!current.Reads.Contains(read)) current.Reads.Add(read);
            }
            return result;
        }

        private static TextureSampleValue DirectValue(Operand value, bool floating, bool guarded, int depth)
        {
            if (depth > 32) return null;
            if (value.Type == OperandType.Constant)
            {
                if (floating && !float.IsFinite(value.AsFloat())) return null;
                return new TextureSampleValue(true, floating, value.Value, 0, 0, guarded);
            }
            if (value.Type == OperandType.ConstantBuffer)
            {
                return new TextureSampleValue(false, floating, 0, value.GetCbufSlot(), value.GetCbufOffset() * 4, guarded);
            }
            if (value.Type == OperandType.LocalVariable && value.AsgOp is Operation copy && copy.Inst == Instruction.Copy)
            {
                return DirectValue(copy.GetSource(0), floating, guarded, depth + 1);
            }
            return null;
        }

        private static TextureSampleValue LayerValue(Operand value, int depth)
        {
            if (depth > 32) return null;
            TextureSampleValue direct = DirectValue(value, false, false, depth);
            if (direct != null) return direct;
            if (value.Type != OperandType.LocalVariable || value.AsgOp is not Operation operation) return null;
            if (operation.Inst == Instruction.Copy) return LayerValue(operation.GetSource(0), depth + 1);
            if (operation.Inst == Instruction.ClampU32 && operation.GetSource(1).Type == OperandType.Constant &&
                operation.GetSource(1).Value == 0 && operation.GetSource(2).Type == OperandType.Constant && operation.GetSource(2).Value == 65535)
            {
                TextureSampleValue converted = LayerValue(operation.GetSource(0), depth + 1);
                return converted != null && converted.IntegerU16 ? converted : null;
            }
            if (operation.Inst is Instruction.ConvertFP32ToU32 or Instruction.ConvertFP32ToS32)
            {
                return GuardedFloatValue(operation.GetSource(0), depth + 1);
            }
            return null;
        }

        private static TextureSampleValue GuardedFloatValue(Operand value, int depth)
        {
            if (depth > 32) return null;
            TextureSampleValue direct = DirectValue(value, true, true, depth);
            if (direct != null) return direct;
            if (value.Type != OperandType.LocalVariable || value.AsgOp is not Operation operation) return null;
            // 有限、整数、0..65535 的运行值使这些转换逐步恒等；guard 不满足时运行时必须使用 unknown。
            // Finite integer runtime values in 0..65535 make every conversion below an identity; runtime must use unknown when the guard fails.
            if (operation.Inst == Instruction.Copy || operation.Inst == (Instruction.Round | Instruction.FP32) ||
                operation.Inst == (Instruction.Floor | Instruction.FP32) || operation.Inst == (Instruction.Ceiling | Instruction.FP32) ||
                operation.Inst == (Instruction.Truncate | Instruction.FP32))
            {
                return GuardedFloatValue(operation.GetSource(0), depth + 1);
            }
            if (operation.Inst == (Instruction.Maximum | Instruction.FP32))
            {
                for (int i = 0; i < 2; i++)
                {
                    Operand zero = operation.GetSource(i);
                    if (zero.Type == OperandType.Constant && zero.AsFloat() == 0f)
                        return GuardedFloatValue(operation.GetSource(1 - i), depth + 1);
                }
            }
            return null;
        }
    }
}
