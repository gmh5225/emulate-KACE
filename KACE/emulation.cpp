#include "emulation.h"
#include <Zydis/Zydis.h>
#include <MemoryTracker/memorytracker.h>
#include <Logger/Logger.h>
#include <assert.h>

#include "paging_emulation.h"
#include "environment.h"
#include "provider.h"

namespace VCPU {

	static ZydisDecoder decoder;
	static ZydisDecodedInstruction instr;

	uint64_t VCPU::CR0 = 0x80050033;
	uint64_t VCPU::CR3 = 0x1ad002;
	uint64_t VCPU::CR4 = 0x370678;
	uint64_t VCPU::CR8 = 0;

	namespace MSRContext {

		std::unordered_map<uint32_t, std::pair<uint64_t, std::string>> MSRData;

		bool Initialize() {
			MSRData.insert(std::pair(0x1D9, std::pair(0, "DBGCTL_MSR")));
			MSRData.insert(std::pair(0x1DB, std::pair(0, "MSRLASTBRANCH-_FROM_IP_MSR")));
			MSRData.insert(std::pair(0x680, std::pair(0, "LastBranchFromIP_MSR")));
			MSRData.insert(std::pair(0x1c9, std::pair(0, "MSR_LASTBRANCH_TOS")));
			MSRData.insert(std::pair(0, std::pair(0xFFF, "MSR_0_P5_IP_ADDR")));
			MSRData.insert(std::pair(0xc0000082, std::pair(0x10000, "MSR_LSTAR")));

			return true;
		}
	}

	void Initialize() {
		ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_ADDRESS_WIDTH_64);
		MemoryTracker::AddMapping(KUSD_MIN, 0x1000, KUSD_USERMODE);
		MemoryTracker::AddMapping(0xFFFFcfe7f3f9f000, 512 * 8, (uintptr_t)&PML4.entries[0]);
		MSRContext::Initialize();
	}

	bool Decode(PCONTEXT context) {
		ZyanU64 runtime_address = context->Rip;
		auto status = ZydisDecoderDecodeBuffer(&decoder, (PVOID)context->Rip, ZYDIS_MAX_INSTRUCTION_LENGTH, &instr);
		return ZYAN_SUCCESS(status);
	}

	static uint32_t GRegIndex(ZydisRegister Reg) {

		PCONTEXT resolver = 0;

		if (Reg == ZYDIS_REGISTER_RIP)
			return (uint32_t)(&resolver->Rip) / 8;
		if (Reg == ZYDIS_REGISTER_EFLAGS)
			return (uint32_t)(&resolver->EFlags) / 8;

		auto lookup = (uint32_t)&resolver->Rax;
		auto zydis_rax = ZYDIS_REGISTER_RAX;

		auto zydis_gr64_lookup = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, Reg);
		auto index = zydis_gr64_lookup - zydis_rax;

		if (index < 0 || index > 15)
			return 0;

		lookup += index * sizeof(uint64_t);

		return lookup / 8;
	}

	static uint64_t ReadRegisterValue(PCONTEXT ctx, ZydisRegister reg) {
		uint64_t* context_lookup = (uint64_t*)ctx;
		auto reg_class = ZydisRegisterGetClass(reg);
		auto ret = context_lookup[GRegIndex(reg)];

		if (reg_class == ZYDIS_REGCLASS_GPR64) { //Read the whole64bit register
			return ret;
		}
		else if (reg_class == ZYDIS_REGCLASS_GPR32) { //32 lower bytes
			return ret & 0xFFFFFFFF;
		}
		else if (reg_class == ZYDIS_REGCLASS_GPR16) { //16 llower bytes
			return ret & 0xFFFF;
		}
		else if (reg_class == ZYDIS_REGCLASS_GPR8) {
			if (reg == ZYDIS_REGISTER_AH || reg == ZYDIS_REGISTER_BH || reg == ZYDIS_REGISTER_CH || reg == ZYDIS_REGISTER_DH) { //8 upper byte
				return (ret & 0xFF00) >> 8;
			}
			else { //8 lower bytes
				return ret & 0xFF;
			}
		}
		else {
			DebugBreak();
		}

		return 0;
	}

	static bool SkipToNext(PCONTEXT ctx) {
		ctx->Rip += instr.length;
		return true;
	}


	namespace PrivilegedInstruction {

		bool Parse(PCONTEXT context) {
			if (!Decode(context))
				return false;

			if (instr.mnemonic == ZYDIS_MNEMONIC_CLI) {
				Logger::Log("Clearing Interrupts\n");
				return SkipToNext(context);
			}
			else if (instr.mnemonic == ZYDIS_MNEMONIC_STI) {
				Logger::Log("Restoring Interrupts\n");
				return SkipToNext(context);
			}
			else if (instr.mnemonic == ZYDIS_MNEMONIC_MOV) {
				EmulatePrivilegedMOV(context);
				return SkipToNext(context);
			}
			else if (instr.mnemonic == ZYDIS_MNEMONIC_WRMSR) {
				if (WriteMSR(context))
					return SkipToNext(context);
				else
					return false;
			}
			else if (instr.mnemonic == ZYDIS_MNEMONIC_RDMSR) {
				if (ReadMSR(context))
					return SkipToNext(context);
				else
					return false;
			}
			else {
				DebugBreak();
				return false;
			}

		}


		bool EmulatePrivilegedMOV(PCONTEXT context) {
			uint64_t* context_lookup = (uint64_t*)context;


			auto reg_to_write = GRegIndex(instr.operands[0].reg.value);
			auto reg_to_read = GRegIndex(instr.operands[1].reg.value);

			if (instr.operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER || instr.operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER) {
				DebugBreak();
			}

			if (!reg_to_read && !reg_to_write) {
				DebugBreak();
			}

			if (instr.operands[0].reg.value == ZYDIS_REGISTER_CR0) { //Write CR0
				Logger::Log("Writing %llx to CR0\n", context_lookup[reg_to_read]);
				VCPU::CR0 = context_lookup[reg_to_read];
			}
			else if (instr.operands[1].reg.value == ZYDIS_REGISTER_CR0) { //Read CR0
				Logger::Log("Reading CR0\n");
				context_lookup[reg_to_write] = VCPU::CR0;
			}
			else if (instr.operands[0].reg.value == ZYDIS_REGISTER_CR3) { //Write CR3
				Logger::Log("Writing %llx to CR3\n", context_lookup[reg_to_read]);
				VCPU::CR3 = context_lookup[reg_to_read];
			}
			else if (instr.operands[1].reg.value == ZYDIS_REGISTER_CR3) { //Read CR3
				Logger::Log("Reading CR3\n");
				context_lookup[reg_to_write] = VCPU::CR3;
			}
			else if (instr.operands[0].reg.value == ZYDIS_REGISTER_CR4) { //Read CR4
				Logger::Log("Writing %llx to CR4\n", context_lookup[reg_to_read]);
				VCPU::CR4 = context_lookup[reg_to_read];
			}
			else if (instr.operands[1].reg.value == ZYDIS_REGISTER_CR4) { //Read CR4
				Logger::Log("Reading CR4\n");
				context_lookup[reg_to_write] = VCPU::CR4;
			}
			else if (instr.operands[0].reg.value == ZYDIS_REGISTER_CR8) { //Write CR8
				Logger::Log("Writing %llx to CR8\n", context_lookup[reg_to_read]);
				VCPU::CR8 = context_lookup[reg_to_read];
			}
			else if (instr.operands[1].reg.value == ZYDIS_REGISTER_CR8) { //Read CR8
				Logger::Log("Reading CR8\n");
				context_lookup[reg_to_write] = VCPU::CR8;
			}
			else if (instr.operands[0].reg.value == ZYDIS_REGISTER_DR7) { //Read CR8
				Logger::Log("Writing %llx to DR7\n", context_lookup[reg_to_read]);
				context->Dr7 = context_lookup[reg_to_read];
			}
			else {
				DebugBreak();
			}

			return true;
		}

		/*
		Reads the contents of a 64-bit model specific register (MSR) specified in the ECX register into registers EDX:EAX. (On processors that support the Intel 64 architecture, the high-order 32 bits of RCX are ignored.) The EDX register is loaded with the high-order 32 bits of the MSR and the EAX register is loaded with the low-order 32 bits. (On processors that support the Intel 64 architecture, the high-order 32 bits of each of RAX and RDX are cleared.) If fewer than 64 bits are implemented in the MSR being read, the values returned to EDX:EAX in unimplemented bit locations are undefined.

		This instruction must be executed at privilege level 0 or in real-address mode; otherwise, a general protection exception #GP(0) will be generated. Specifying a reserved or unimplemented MSR address in ECX will also cause a general protection exception.
		*/

		bool ReadMSR(PCONTEXT context) {
			uint32_t ECX = context->Rcx & 0xFFFFFFFF;

			if (!MSRContext::MSRData.contains(ECX)) {
				Logger::Log("Reading from unsupported MSR : %llx\n", ECX);
				return false;
			}

			auto ReadData = MSRContext::MSRData[ECX];
			auto MSRValue = ReadData.first;
			auto MSRName = ReadData.second;
			context->Rdx = (MSRValue >> 32) & 0xFFFFFFFF;
			context->Rax = (MSRValue) & 0xFFFFFFFF;
			Logger::Log("Reading MSR %s : %llx\n", MSRName.c_str(), MSRValue);
			return true;
		}

		bool WriteMSR(PCONTEXT context) {
			uint32_t ECX = context->Rcx & 0xFFFFFFFF;

			if (!MSRContext::MSRData.contains(ECX)) { //GP(0) If the value in ECX specifies a reserved or unimplemented MSR address
				Logger::Log("Writing to unsupported MSR : %llx\n", ECX);
				return false;
			}

			auto ReadData = MSRContext::MSRData[ECX];
			auto MSRValue = ReadData.first;
			auto MSRName = ReadData.second;

			auto NewMSRValue = (context->Rdx << 32) | (context->Rax) & 0xFFFFFFFF;

			MSRContext::MSRData[ECX] = std::pair(NewMSRValue, MSRName);

			Logger::Log("Writing MSR %s : %llx\n", MSRName.c_str(), NewMSRValue);
			return true;
		}


	}

	namespace MemoryWrite {

		bool Parse(uintptr_t addr, PCONTEXT context) {
			if (!Decode(context))
				return false;

			if (auto exportImpl = Provider::FindDataImpl(addr)) {
				return EmulateWrite(exportImpl, context);
			}

			if (auto HVA = MemoryTracker::GetHVA(addr)) {
				printf("Emulating write to %llx translated to %llx\n", addr, HVA);
				return EmulateWrite(HVA, context);
			}
			else {
				if (addr == 0xffffffffffffffff)
					return false;
				Environment::CheckPtr(addr);
				Logger::Log("Logging from a memory that has no usermode mapping : %llx\n", addr);
				fflush(stdout);
				return false;
			}
		}

		bool EmulateWrite(uintptr_t addr, PCONTEXT context) { //We return true if we emulated it

			if (instr.mnemonic == ZYDIS_MNEMONIC_MOV) {
				if (instr.operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
					InstrEmu::WritePtr::EmulateMOV(context, instr.operands[1].reg.value, addr);
					return SkipToNext(context);
				}
				else {
					Logger::Log("This should never happen, please investigate\n");
					DebugBreak();
				}
			}
			else if (instr.mnemonic == ZYDIS_MNEMONIC_OR) {
				if (instr.operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
					InstrEmu::WritePtr::EmulateOR(context, instr.operands[1].reg.value, addr);
					return SkipToNext(context);
				}
				else {

					DebugBreak();
				}
			}
			else if (instr.mnemonic == ZYDIS_MNEMONIC_XOR) {
				if (instr.operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
					InstrEmu::WritePtr::EmulateXOR(context, instr.operands[1].reg.value, addr);
					return SkipToNext(context);
				}
				else {

					DebugBreak();
				}
			}
			else if (instr.mnemonic == ZYDIS_MNEMONIC_AND) {
				if (instr.operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {

					InstrEmu::WritePtr::EmulateAND(context, instr.operands[1].reg.value, addr);
					return SkipToNext(context);
				}
				else {

					DebugBreak();
				}
			}
			else {
				Logger::Log("Unhandled Mnemonic.\n");
				DebugBreak();
				return false;
			}
			return false;
		}


	}


	namespace MemoryRead {

		bool Parse(uintptr_t addr, PCONTEXT context) {
			if (!Decode(context))
				return false;


			if (auto exportImpl = Provider::FindDataImpl(addr)) {
				return EmulateRead(exportImpl, context);
			}

			if (auto HVA = MemoryTracker::GetHVA(addr)) {
				printf("Emulating read from %llx translated to %llx\n", addr, HVA);
				return EmulateRead(HVA, context);
			}
			else {
				if (addr == 0xffffffffffffffff)
					return false;
				Environment::CheckPtr(addr);
				Logger::Log("Logging from a memory that has no usermode mapping : %llx\n", addr);
				fflush(stdout);
				return false;
			}
		}

		bool EmulateRead(uintptr_t addr, PCONTEXT context) { //We return true if we emulated it

			if (instr.mnemonic == ZYDIS_MNEMONIC_MOV) {
				if (instr.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
					InstrEmu::ReadPtr::EmulateMOV(context, instr.operands[0].reg.value, addr);
					return SkipToNext(context);
				}
				else {
					Logger::Log("This should never happen, please investigate\n");
					DebugBreak();
				}
			}
			else if (instr.mnemonic == ZYDIS_MNEMONIC_OR) {
				if (instr.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
					InstrEmu::ReadPtr::EmulateOR(context, instr.operands[0].reg.value, addr);
					return SkipToNext(context);
				}
				else {

					DebugBreak();
				}
			}
			else if (instr.mnemonic == ZYDIS_MNEMONIC_XOR) {
				if (instr.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
					InstrEmu::ReadPtr::EmulateXOR(context, instr.operands[0].reg.value, addr);
					return SkipToNext(context);
				}
				else {

					DebugBreak();
				}
			}
			else if (instr.mnemonic == ZYDIS_MNEMONIC_AND) {
				if (instr.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {

					InstrEmu::ReadPtr::EmulateAND(context, instr.operands[0].reg.value, addr);
					return SkipToNext(context);
				}
				else {

					DebugBreak();
				}
			}
			else if (instr.mnemonic == ZYDIS_MNEMONIC_SUB) {
				if (instr.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {

					InstrEmu::ReadPtr::EmulateSUB(context, instr.operands[0].reg.value, addr);
					return SkipToNext(context);
				}
				else {

					DebugBreak();
				}
			}
			else if (instr.mnemonic == ZYDIS_MNEMONIC_ADD) {
				if (instr.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {

					InstrEmu::ReadPtr::EmulateADD(context, instr.operands[0].reg.value, addr);
					return SkipToNext(context);
				}
				else {

					DebugBreak();
				}
			}
			else if (instr.mnemonic == ZYDIS_MNEMONIC_CMP) {
				if (instr.operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY && instr.operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER) { //cmp [memory], reg
					InstrEmu::EmulateCMPSourcePtr(context, instr.operands[1].reg.value, addr);
					return SkipToNext(context);
				}
				else if (instr.operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY && instr.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER) { //cmp reg, [memory]
					InstrEmu::EmulateCMPDestPtr(context, instr.operands[0].reg.value, addr);
					return SkipToNext(context);
				}
				else if (instr.operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY && instr.operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) { //cmp reg, [memory]
					InstrEmu::EmulateCMPImm(context, instr.operands[0].imm.value.s, addr, instr.operands[1].element_size);
					return SkipToNext(context);
				}
				else if (instr.operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && instr.operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY) { //cmp reg, [memory]
					InstrEmu::EmulateCMPImm(context, instr.operands[1].imm.value.s, addr, instr.operands[0].element_size);
					return SkipToNext(context);
				}
				else {
					DebugBreak();
				}
			}
			else if (instr.mnemonic == ZYDIS_MNEMONIC_TEST) {
				if (instr.operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY && instr.operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER) { //cmp [memory], reg
					InstrEmu::EmulateTestSourcePtr(context, instr.operands[1].reg.value, addr);
					return SkipToNext(context);
				}
				else if (instr.operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY && instr.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER) { //cmp reg, [memory]
					InstrEmu::EmulateTestDestPtr(context, instr.operands[0].reg.value, addr);
					return SkipToNext(context);
				}
				else if (instr.operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY && instr.operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) { //cmp reg, [memory]
					InstrEmu::EmulateTestImm(context, instr.operands[0].imm.value.s, addr, instr.operands[1].element_size);
					return SkipToNext(context);
				}
				else if (instr.operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && instr.operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY) { //cmp reg, [memory]
					InstrEmu::EmulateTestImm(context, instr.operands[1].imm.value.s, addr, instr.operands[0].element_size);
					return SkipToNext(context);
				}
				else {
					DebugBreak();
				}
			}
			else if (instr.mnemonic == ZYDIS_MNEMONIC_MOVZX) {
				if (instr.operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY && instr.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER) { //cmp reg, [memory]
					InstrEmu::ReadPtr::EmulateMOVZX(context, instr.operands[0].reg.value, addr, instr.operands[1].size);
					return SkipToNext(context);
				}
				else {
					DebugBreak();
				}
			}
			else if (instr.mnemonic == ZYDIS_MNEMONIC_MOVSXD) {
				if (instr.operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY && instr.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER) { //cmp reg, [memory]
					InstrEmu::ReadPtr::EmulateMOVSX(context, instr.operands[0].reg.value, addr, instr.operands[1].size);
					return SkipToNext(context);
				}
				else {
					DebugBreak();
				}
			}

			else {
				Logger::Log("Unhandled Mnemonic for KUSER_SHARED_DATA manipulation.\n");
				DebugBreak();
				return false;
			}
			return false;
		}


	}


	namespace InstrEmu {

		bool EmulateCMPSourcePtr(PCONTEXT ctx, ZydisRegister reg, uint64_t ptr) { //Emulates cmp [ptr], reg // cmp reg, [ptr]

			uint64_t* context_lookup = (uint64_t*)ctx;
			auto reg_class = ZydisRegisterGetClass(reg);
			auto reg_value = ReadRegisterValue(ctx, reg);

			if (reg_class == ZYDIS_REGCLASS_GPR64) {
				ctx->EFlags = u_cmp_64_sp(ctx->EFlags, ptr, reg_value);
			}
			else if (reg_class == ZYDIS_REGCLASS_GPR32) {
				ctx->EFlags = u_cmp_32_sp(ctx->EFlags, ptr, reg_value);
			}
			else if (reg_class == ZYDIS_REGCLASS_GPR16) {
				ctx->EFlags = u_cmp_16_sp(ctx->EFlags, ptr, reg_value);

			}
			else if (reg_class == ZYDIS_REGCLASS_GPR8) {
				ctx->EFlags = u_cmp_8_sp(ctx->EFlags, ptr, reg_value);
			}
			else {
				DebugBreak();
			}
			return true;
		}

		bool EmulateCMPDestPtr(PCONTEXT ctx, ZydisRegister reg, uint64_t ptr) { //Emulates cmp [ptr], reg // cmp reg, [ptr]

			uint64_t* context_lookup = (uint64_t*)ctx;
			auto reg_class = ZydisRegisterGetClass(reg);
			auto reg_value = ReadRegisterValue(ctx, reg);

			if (reg_class == ZYDIS_REGCLASS_GPR64) {
				ctx->EFlags = u_cmp_64_dp(ctx->EFlags, ptr, reg_value);
			}
			else if (reg_class == ZYDIS_REGCLASS_GPR32) {
				ctx->EFlags = u_cmp_32_dp(ctx->EFlags, ptr, reg_value);
			}
			else if (reg_class == ZYDIS_REGCLASS_GPR16) {
				ctx->EFlags = u_cmp_16_dp(ctx->EFlags, ptr, reg_value);

			}
			else if (reg_class == ZYDIS_REGCLASS_GPR8) {
				ctx->EFlags = u_cmp_8_dp(ctx->EFlags, ptr, reg_value);
			}
			else {
				DebugBreak();
			}
			return true;
		}


		bool EmulateCMPImm(PCONTEXT ctx, int32_t imm, uint64_t ptr, size_t size) { //Emulates cmp [ptr], reg // cmp reg, [ptr]

			uint64_t* context_lookup = (uint64_t*)ctx;

			if (size == 64) {
				ctx->EFlags = u_cmp_64_sp(ctx->EFlags, ptr, imm);
			}
			else if (size == 32) {
				ctx->EFlags = u_cmp_32_sp(ctx->EFlags, ptr, imm);
			}
			else if (size == 16) {
				ctx->EFlags = u_cmp_16_sp(ctx->EFlags, ptr, imm);

			}
			else if (size == 8) {
				ctx->EFlags = u_cmp_8_sp(ctx->EFlags, ptr, imm);
			}
			else {
				DebugBreak();
			}
			return true;
		}


		bool EmulateTestSourcePtr(PCONTEXT ctx, ZydisRegister reg, uint64_t ptr) { //Emulates cmp [ptr], reg // cmp reg, [ptr]

			uint64_t* context_lookup = (uint64_t*)ctx;
			auto reg_class = ZydisRegisterGetClass(reg);
			auto reg_value = ReadRegisterValue(ctx, reg);

			if (reg_class == ZYDIS_REGCLASS_GPR64) {
				ctx->EFlags = u_test_64_sp(ctx->EFlags, ptr, reg_value) | 0x10000;
			}
			else if (reg_class == ZYDIS_REGCLASS_GPR32) {
				ctx->EFlags = u_test_32_sp(ctx->EFlags, ptr, reg_value) | 0x10000;
			}
			else if (reg_class == ZYDIS_REGCLASS_GPR16) {
				ctx->EFlags = u_test_16_sp(ctx->EFlags, ptr, reg_value) | 0x10000;

			}
			else if (reg_class == ZYDIS_REGCLASS_GPR8) {
				ctx->EFlags = u_test_8_sp(ctx->EFlags, ptr, reg_value) | 0x10000;
			}
			else {
				DebugBreak();
			}
			return true;
		}


		bool EmulateTestDestPtr(PCONTEXT ctx, ZydisRegister reg, uint64_t ptr) { //Emulates cmp [ptr], reg // cmp reg, [ptr]

			uint64_t* context_lookup = (uint64_t*)ctx;
			auto reg_class = ZydisRegisterGetClass(reg);
			auto reg_value = ReadRegisterValue(ctx, reg);

			if (reg_class == ZYDIS_REGCLASS_GPR64) {
				ctx->EFlags = u_test_64_dp(ctx->EFlags, ptr, reg_value) | 0x10000;
			}
			else if (reg_class == ZYDIS_REGCLASS_GPR32) {
				ctx->EFlags = u_test_32_dp(ctx->EFlags, ptr, reg_value) | 0x10000;
			}
			else if (reg_class == ZYDIS_REGCLASS_GPR16) {
				ctx->EFlags = u_test_16_dp(ctx->EFlags, ptr, reg_value) | 0x10000;

			}
			else if (reg_class == ZYDIS_REGCLASS_GPR8) {
				ctx->EFlags = u_test_8_dp(ctx->EFlags, ptr, reg_value) | 0x10000;
			}
			else {
				DebugBreak();
			}
			return true;
		}


		bool EmulateTestImm(PCONTEXT ctx, int32_t imm, uint64_t ptr, size_t size) { //Emulates cmp [ptr], reg // cmp reg, [ptr]

			uint64_t* context_lookup = (uint64_t*)ctx;

			if (size == 64) {
				ctx->EFlags = u_test_64_sp(ctx->EFlags, ptr, imm) | 0x10000;
			}
			else if (size == 32) {
				ctx->EFlags = u_test_32_sp(ctx->EFlags, ptr, imm) | 0x10000;
			}
			else if (size == 16) {
				ctx->EFlags = u_test_16_sp(ctx->EFlags, ptr, imm) | 0x10000;

			}
			else if (size == 8) {
				ctx->EFlags = u_test_8_sp(ctx->EFlags, ptr, imm) | 0x10000;
			}
			else {
				DebugBreak();
			}
			return true;
		}

		namespace ReadPtr {
			bool EmulateMOV(PCONTEXT ctx, ZydisRegister reg, uint64_t ptr) { //X86-compliant MOV R64, [...] emulation

				uint64_t* context_lookup = (uint64_t*)ctx;
				auto reg_class = ZydisRegisterGetClass(reg);
				auto orig_value = context_lookup[GRegIndex(reg)];


				if (reg_class == ZYDIS_REGCLASS_GPR64) { //We replace the whole register
					context_lookup[GRegIndex(reg)] = *(uint64_t*)ptr;

				}
				else if (reg_class == ZYDIS_REGCLASS_GPR32) { //We replace the whole register
					context_lookup[GRegIndex(reg)] = *(uint32_t*)ptr;
				}
				else if (reg_class == ZYDIS_REGCLASS_GPR16) { // 16/8bits operation do not overwrite the rest of the register
					context_lookup[GRegIndex(reg)] = (orig_value & 0xFFFFFFFFFFFF0000) | (*(uint16_t*)ptr);

				}
				else if (reg_class == ZYDIS_REGCLASS_GPR8) {  // 16/8bits operation do not overwrite the rest of the register
					if (reg == ZYDIS_REGISTER_AH || reg == ZYDIS_REGISTER_BH || reg == ZYDIS_REGISTER_CH || reg == ZYDIS_REGISTER_DH) {
						context_lookup[GRegIndex(reg)] = (orig_value & 0xFFFFFFFFFFFF00FF) | (*(uint8_t*)ptr) << 8;
					}
					else {  // 16/8bits operation do not overwrite the rest of the register
						context_lookup[GRegIndex(reg)] = (orig_value & 0xFFFFFFFFFFFFFF00) | (*(uint8_t*)ptr);
					}
				}
				else {
					DebugBreak();
				}
				return true;
			}


			bool EmulateSUB(PCONTEXT ctx, ZydisRegister reg, uint64_t ptr) { //X86-compliant MOV R64, [...] emulation
				uint64_t* context_lookup = (uint64_t*)ctx;
				auto reg_class = ZydisRegisterGetClass(reg);


				if (reg_class == ZYDIS_REGCLASS_GPR64) {
					context_lookup[GRegIndex(reg)] -= *(uint64_t*)ptr;

				}
				else if (reg_class == ZYDIS_REGCLASS_GPR32) { //r32/m32 removes upper byte
					context_lookup[GRegIndex(reg)] = (context_lookup[GRegIndex(reg)] & 0xFFFFFFFF) - *(uint32_t*)ptr;
				}
				else if (reg_class == ZYDIS_REGCLASS_GPR16) {
					context_lookup[GRegIndex(reg)] -= (*(uint16_t*)ptr);

				}
				else if (reg_class == ZYDIS_REGCLASS_GPR8) {
					if (reg == ZYDIS_REGISTER_AH || reg == ZYDIS_REGISTER_BH || reg == ZYDIS_REGISTER_CH || reg == ZYDIS_REGISTER_DH) {
						context_lookup[GRegIndex(reg)] -= (*(uint8_t*)ptr) << 8;
					}
					else {
						context_lookup[GRegIndex(reg)] -= (*(uint8_t*)ptr);
					}
				}
				else {
					DebugBreak();
				}
				return true;
			}

			bool EmulateADD(PCONTEXT ctx, ZydisRegister reg, uint64_t ptr) { //X86-compliant MOV R64, [...] emulation
				uint64_t* context_lookup = (uint64_t*)ctx;
				auto reg_class = ZydisRegisterGetClass(reg);


				if (reg_class == ZYDIS_REGCLASS_GPR64) {
					context_lookup[GRegIndex(reg)] += *(uint64_t*)ptr;

				}
				else if (reg_class == ZYDIS_REGCLASS_GPR32) { //r32/m32 removes upper byte
					context_lookup[GRegIndex(reg)] = (context_lookup[GRegIndex(reg)] & 0xFFFFFFFF) + *(uint32_t*)ptr;
				}
				else if (reg_class == ZYDIS_REGCLASS_GPR16) {
					context_lookup[GRegIndex(reg)] += (*(uint16_t*)ptr);

				}
				else if (reg_class == ZYDIS_REGCLASS_GPR8) {
					if (reg == ZYDIS_REGISTER_AH || reg == ZYDIS_REGISTER_BH || reg == ZYDIS_REGISTER_CH || reg == ZYDIS_REGISTER_DH) {
						context_lookup[GRegIndex(reg)] += (*(uint8_t*)ptr) << 8;
					}
					else {
						context_lookup[GRegIndex(reg)] += (*(uint8_t*)ptr);
					}
				}
				else {
					DebugBreak();
				}
				return true;
			}

			bool EmulateOR(PCONTEXT ctx, ZydisRegister reg, uint64_t ptr) { //X86-compliant MOV R64, [...] emulation
				uint64_t* context_lookup = (uint64_t*)ctx;
				auto reg_class = ZydisRegisterGetClass(reg);


				if (reg_class == ZYDIS_REGCLASS_GPR64) {
					context_lookup[GRegIndex(reg)] |= *(uint64_t*)ptr;

				}
				else if (reg_class == ZYDIS_REGCLASS_GPR32) { //r32/m32 removes upper byte
					context_lookup[GRegIndex(reg)] = (context_lookup[GRegIndex(reg)] & 0xFFFFFFFF) | *(uint32_t*)ptr;
				}
				else if (reg_class == ZYDIS_REGCLASS_GPR16) {
					context_lookup[GRegIndex(reg)] |= (*(uint16_t*)ptr);

				}
				else if (reg_class == ZYDIS_REGCLASS_GPR8) {
					if (reg == ZYDIS_REGISTER_AH || reg == ZYDIS_REGISTER_BH || reg == ZYDIS_REGISTER_CH || reg == ZYDIS_REGISTER_DH) {
						context_lookup[GRegIndex(reg)] |= (*(uint8_t*)ptr) << 8;
					}
					else {
						context_lookup[GRegIndex(reg)] |= (*(uint8_t*)ptr);
					}
				}
				else {
					DebugBreak();
				}
				return true;
			}

			bool EmulateXOR(PCONTEXT ctx, ZydisRegister reg, uint64_t ptr) { //X86-compliant MOV R64, [...] emulation
				uint64_t* context_lookup = (uint64_t*)ctx;
				auto reg_class = ZydisRegisterGetClass(reg);


				if (reg_class == ZYDIS_REGCLASS_GPR64) {
					context_lookup[GRegIndex(reg)] ^= *(uint64_t*)ptr;

				}
				else if (reg_class == ZYDIS_REGCLASS_GPR32) { //r32/m32 removes upper byte
					context_lookup[GRegIndex(reg)] = (context_lookup[GRegIndex(reg)] & 0xFFFFFFFF) ^ *(uint32_t*)ptr;
				}
				else if (reg_class == ZYDIS_REGCLASS_GPR16) {
					context_lookup[GRegIndex(reg)] ^= (*(uint16_t*)ptr);

				}
				else if (reg_class == ZYDIS_REGCLASS_GPR8) {
					if (reg == ZYDIS_REGISTER_AH || reg == ZYDIS_REGISTER_BH || reg == ZYDIS_REGISTER_CH || reg == ZYDIS_REGISTER_DH) {
						context_lookup[GRegIndex(reg)] ^= (*(uint8_t*)ptr) << 8;
					}
					else {
						context_lookup[GRegIndex(reg)] ^= (*(uint8_t*)ptr);
					}
				}
				else {
					DebugBreak();
				}
				return true;
			}

			bool EmulateAND(PCONTEXT ctx, ZydisRegister reg, uint64_t ptr) { //X86-compliant MOV R64, [...] emulation
				uint64_t* context_lookup = (uint64_t*)ctx;
				auto reg_class = ZydisRegisterGetClass(reg);

				if (reg_class == ZYDIS_REGCLASS_GPR64) {
					context_lookup[GRegIndex(reg)] &= *(uint64_t*)ptr;
				}
				else if (reg_class == ZYDIS_REGCLASS_GPR32) { //r32/m32 removes upper byte
					context_lookup[GRegIndex(reg)] = (context_lookup[GRegIndex(reg)] & 0xFFFFFFFF) & *(uint32_t*)ptr;
				}
				else if (reg_class == ZYDIS_REGCLASS_GPR16) {
					context_lookup[GRegIndex(reg)] &= (*(uint16_t*)ptr);
				}
				else if (reg_class == ZYDIS_REGCLASS_GPR8) {
					if (reg == ZYDIS_REGISTER_AH || reg == ZYDIS_REGISTER_BH || reg == ZYDIS_REGISTER_CH || reg == ZYDIS_REGISTER_DH) {
						context_lookup[GRegIndex(reg)] &= (*(uint8_t*)ptr) << 8;
					}
					else {
						context_lookup[GRegIndex(reg)] &= (*(uint8_t*)ptr);
					}
				}
				else {
					DebugBreak();
				}
				return true;
			}

			bool EmulateMOVZX(PCONTEXT ctx, ZydisRegister reg, uint64_t ptr, uint32_t size) { //X86-compliant MOVZX R32/16, 8/16[PTR] emulation

				uint64_t* context_lookup = (uint64_t*)ctx;
				auto reg_class = ZydisRegisterGetClass(reg);
				auto orig_value = context_lookup[GRegIndex(reg)];



				if (reg_class == ZYDIS_REGCLASS_GPR32) { //We replace the whole register
					if (size == 16) {
						context_lookup[GRegIndex(reg)] = *(uint16_t*)ptr;
					}
					else if (size == 8) {
						context_lookup[GRegIndex(reg)] = *(uint8_t*)ptr;
					}
					else {
						DebugBreak();
					}

				}
				else if (reg_class == ZYDIS_REGCLASS_GPR16) { // 16/8bits operation do not overwrite the rest of the register
					if (size == 8) {
						context_lookup[GRegIndex(reg)] = *(uint8_t*)ptr;
					}
					else {
						DebugBreak();
					}


				}
				else {
					DebugBreak();
				}
				return true;
			}

			bool EmulateMOVSX(PCONTEXT ctx, ZydisRegister reg, uint64_t ptr, uint32_t size) { //X86-compliant MOVZX R32/16, 8/16[PTR] emulation

				uint64_t* context_lookup = (uint64_t*)ctx;
				auto reg_class = ZydisRegisterGetClass(reg);
				auto orig_value = context_lookup[GRegIndex(reg)];

				if (size == 64) {
					context_lookup[GRegIndex(reg)] = *(int64_t*)ptr;
				}
				else if (size == 32) {
					context_lookup[GRegIndex(reg)] = *(int32_t*)ptr;
				}

				else if (size == 16) {
					context_lookup[GRegIndex(reg)] = *(int16_t*)ptr;
				}
				else if (size == 8) {
					context_lookup[GRegIndex(reg)] = *(int8_t*)ptr;
				}

				else {
					DebugBreak();
				}
				return true;
			}

		}


		namespace WritePtr {

			bool EmulateMOV(PCONTEXT ctx, ZydisRegister reg, uint64_t ptr) { //X86-compliant MOV R64, [...] emulation

				uint64_t* context_lookup = (uint64_t*)ctx;
				auto reg_class = ZydisRegisterGetClass(reg);
				auto orig_value = context_lookup[GRegIndex(reg)];


				if (reg_class == ZYDIS_REGCLASS_GPR64) { //We replace the whole register
					*(uint64_t*)ptr = context_lookup[GRegIndex(reg)];

				}
				else if (reg_class == ZYDIS_REGCLASS_GPR32) { //We replace the whole register
					*(uint32_t*)ptr = context_lookup[GRegIndex(reg)];
				}
				else if (reg_class == ZYDIS_REGCLASS_GPR16) { // 16/8bits operation do not overwrite the rest of the register
					*(uint16_t*)ptr = context_lookup[GRegIndex(reg)];

				}
				else if (reg_class == ZYDIS_REGCLASS_GPR8) {  // 16/8bits operation do not overwrite the rest of the register
					if (reg == ZYDIS_REGISTER_AH || reg == ZYDIS_REGISTER_BH || reg == ZYDIS_REGISTER_CH || reg == ZYDIS_REGISTER_DH) {
						*(uint8_t*)ptr = context_lookup[GRegIndex(reg)] >> 8;
					}
					else {  // 16/8bits operation do not overwrite the rest of the register
						*(uint8_t*)ptr = context_lookup[GRegIndex(reg)];
					}
				}
				else {
					DebugBreak();
				}
				return true;
			}


			bool EmulateSUB(PCONTEXT ctx, ZydisRegister reg, uint64_t ptr) { //X86-compliant MOV R64, [...] emulation
				uint64_t* context_lookup = (uint64_t*)ctx;
				auto reg_class = ZydisRegisterGetClass(reg);


				DebugBreak();
				return true;
			}

			bool EmulateADD(PCONTEXT ctx, ZydisRegister reg, uint64_t ptr) { //X86-compliant MOV R64, [...] emulation
				uint64_t* context_lookup = (uint64_t*)ctx;
				auto reg_class = ZydisRegisterGetClass(reg);


				DebugBreak();
				return true;
			}

			bool EmulateOR(PCONTEXT ctx, ZydisRegister reg, uint64_t ptr) { //X86-compliant MOV R64, [...] emulation
				uint64_t* context_lookup = (uint64_t*)ctx;
				auto reg_class = ZydisRegisterGetClass(reg);


				DebugBreak();
				return true;
			}

			bool EmulateXOR(PCONTEXT ctx, ZydisRegister reg, uint64_t ptr) { //X86-compliant MOV R64, [...] emulation
				uint64_t* context_lookup = (uint64_t*)ctx;
				auto reg_class = ZydisRegisterGetClass(reg);


				DebugBreak();
				return true;
			}

			bool EmulateAND(PCONTEXT ctx, ZydisRegister reg, uint64_t ptr) { //X86-compliant MOV R64, [...] emulation
				uint64_t* context_lookup = (uint64_t*)ctx;
				auto reg_class = ZydisRegisterGetClass(reg);

				DebugBreak();
				return true;
			}

			bool EmulateMOVZX(PCONTEXT ctx, ZydisRegister reg, uint64_t ptr, uint32_t size) { //X86-compliant MOVZX R32/16, 8/16[PTR] emulation

				uint64_t* context_lookup = (uint64_t*)ctx;
				auto reg_class = ZydisRegisterGetClass(reg);
				auto orig_value = context_lookup[GRegIndex(reg)];

				DebugBreak();
				return true;
			}

		}
	}
}
