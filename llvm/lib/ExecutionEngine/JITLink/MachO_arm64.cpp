//===---- MachO_arm64.cpp - JIT linker implementation for MachO/arm64 -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// MachO/arm64 jit-link implementation.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/JITLink/MachO_arm64.h"

#include "BasicGOTAndStubsBuilder.h"
#include "MachOLinkGraphBuilder.h"

#define DEBUG_TYPE "jitlink"

using namespace llvm;
using namespace llvm::jitlink;
using namespace llvm::jitlink::MachO_arm64_Edges;

namespace {

class MachOLinkGraphBuilder_arm64 : public MachOLinkGraphBuilder {
public:
  MachOLinkGraphBuilder_arm64(const object::MachOObjectFile &Obj)
      : MachOLinkGraphBuilder(Obj),
        NumSymbols(Obj.getSymtabLoadCommand().nsyms) {
    addCustomSectionParser(
        "__eh_frame", [this](NormalizedSection &EHFrameSection) {
          if (!EHFrameSection.Data)
            return make_error<JITLinkError>(
                "__eh_frame section is marked zero-fill");
          return MachOEHFrameBinaryParser(
                     *this, EHFrameSection.Address,
                     StringRef(EHFrameSection.Data, EHFrameSection.Size),
                     *EHFrameSection.GraphSection, 8, 4, NegDelta32, Delta64)
              .addToGraph();
        });
  }

private:
  static Expected<MachOARM64RelocationKind>
  getRelocationKind(const MachO::relocation_info &RI) {
    switch (RI.r_type) {
    case MachO::ARM64_RELOC_UNSIGNED:
      if (!RI.r_pcrel) {
        if (RI.r_length == 3)
          return RI.r_extern ? Pointer64 : Pointer64Anon;
        else if (RI.r_length == 2)
          return Pointer32;
      }
      break;
    case MachO::ARM64_RELOC_SUBTRACTOR:
      // SUBTRACTOR must be non-pc-rel, extern, with length 2 or 3.
      // Initially represent SUBTRACTOR relocations with 'Delta<W>'.
      // They may be turned into NegDelta<W> by parsePairRelocation.
      if (!RI.r_pcrel && RI.r_extern) {
        if (RI.r_length == 2)
          return Delta32;
        else if (RI.r_length == 3)
          return Delta64;
      }
      break;
    case MachO::ARM64_RELOC_BRANCH26:
      if (RI.r_pcrel && RI.r_extern && RI.r_length == 2)
        return Branch26;
      break;
    case MachO::ARM64_RELOC_PAGE21:
      if (RI.r_pcrel && RI.r_extern && RI.r_length == 2)
        return Page21;
      break;
    case MachO::ARM64_RELOC_PAGEOFF12:
      if (!RI.r_pcrel && RI.r_extern && RI.r_length == 2)
        return PageOffset12;
      break;
    case MachO::ARM64_RELOC_GOT_LOAD_PAGE21:
      if (RI.r_pcrel && RI.r_extern && RI.r_length == 2)
        return GOTPage21;
      break;
    case MachO::ARM64_RELOC_GOT_LOAD_PAGEOFF12:
      if (!RI.r_pcrel && RI.r_extern && RI.r_length == 2)
        return GOTPageOffset12;
      break;
    case MachO::ARM64_RELOC_POINTER_TO_GOT:
      if (RI.r_pcrel && RI.r_extern && RI.r_length == 2)
        return PointerToGOT;
      break;
    case MachO::ARM64_RELOC_ADDEND:
      if (!RI.r_pcrel && !RI.r_extern && RI.r_length == 2)
        return PairedAddend;
      break;
    }

    return make_error<JITLinkError>(
        "Unsupported arm64 relocation: address=" +
        formatv("{0:x8}", RI.r_address) +
        ", symbolnum=" + formatv("{0:x6}", RI.r_symbolnum) +
        ", kind=" + formatv("{0:x1}", RI.r_type) +
        ", pc_rel=" + (RI.r_pcrel ? "true" : "false") +
        ", extern=" + (RI.r_extern ? "true" : "false") +
        ", length=" + formatv("{0:d}", RI.r_length));
  }

  MachO::relocation_info
  getRelocationInfo(const object::relocation_iterator RelItr) {
    MachO::any_relocation_info ARI =
        getObject().getRelocation(RelItr->getRawDataRefImpl());
    MachO::relocation_info RI;
    memcpy(&RI, &ARI, sizeof(MachO::relocation_info));
    return RI;
  }

  using PairRelocInfo =
      std::tuple<MachOARM64RelocationKind, Symbol *, uint64_t>;

  // Parses paired SUBTRACTOR/UNSIGNED relocations and, on success,
  // returns the edge kind and addend to be used.
  Expected<PairRelocInfo>
  parsePairRelocation(Block &BlockToFix, Edge::Kind SubtractorKind,
                      const MachO::relocation_info &SubRI,
                      JITTargetAddress FixupAddress, const char *FixupContent,
                      object::relocation_iterator &UnsignedRelItr,
                      object::relocation_iterator &RelEnd) {
    using namespace support;

    assert(((SubtractorKind == Delta32 && SubRI.r_length == 2) ||
            (SubtractorKind == Delta64 && SubRI.r_length == 3)) &&
           "Subtractor kind should match length");
    assert(SubRI.r_extern && "SUBTRACTOR reloc symbol should be extern");
    assert(!SubRI.r_pcrel && "SUBTRACTOR reloc should not be PCRel");

    if (UnsignedRelItr == RelEnd)
      return make_error<JITLinkError>("arm64 SUBTRACTOR without paired "
                                      "UNSIGNED relocation");

    auto UnsignedRI = getRelocationInfo(UnsignedRelItr);

    if (SubRI.r_address != UnsignedRI.r_address)
      return make_error<JITLinkError>("arm64 SUBTRACTOR and paired UNSIGNED "
                                      "point to different addresses");

    if (SubRI.r_length != UnsignedRI.r_length)
      return make_error<JITLinkError>("length of arm64 SUBTRACTOR and paired "
                                      "UNSIGNED reloc must match");

    Symbol *FromSymbol;
    if (auto FromSymbolOrErr = findSymbolByIndex(SubRI.r_symbolnum))
      FromSymbol = FromSymbolOrErr->GraphSymbol;
    else
      return FromSymbolOrErr.takeError();

    // Read the current fixup value.
    uint64_t FixupValue = 0;
    if (SubRI.r_length == 3)
      FixupValue = *(const little64_t *)FixupContent;
    else
      FixupValue = *(const little32_t *)FixupContent;

    // Find 'ToSymbol' using symbol number or address, depending on whether the
    // paired UNSIGNED relocation is extern.
    Symbol *ToSymbol = nullptr;
    if (UnsignedRI.r_extern) {
      // Find target symbol by symbol index.
      if (auto ToSymbolOrErr = findSymbolByIndex(UnsignedRI.r_symbolnum))
        ToSymbol = ToSymbolOrErr->GraphSymbol;
      else
        return ToSymbolOrErr.takeError();
    } else {
      if (auto ToSymbolOrErr = findSymbolByAddress(FixupValue))
        ToSymbol = &*ToSymbolOrErr;
      else
        return ToSymbolOrErr.takeError();
      FixupValue -= ToSymbol->getAddress();
    }

    MachOARM64RelocationKind DeltaKind;
    Symbol *TargetSymbol;
    uint64_t Addend;
    if (&BlockToFix == &FromSymbol->getAddressable()) {
      TargetSymbol = ToSymbol;
      DeltaKind = (SubRI.r_length == 3) ? Delta64 : Delta32;
      Addend = FixupValue + (FixupAddress - FromSymbol->getAddress());
      // FIXME: handle extern 'from'.
    } else if (&BlockToFix == &ToSymbol->getAddressable()) {
      TargetSymbol = &*FromSymbol;
      DeltaKind = (SubRI.r_length == 3) ? NegDelta64 : NegDelta32;
      Addend = FixupValue - (FixupAddress - ToSymbol->getAddress());
    } else {
      // BlockToFix was neither FromSymbol nor ToSymbol.
      return make_error<JITLinkError>("SUBTRACTOR relocation must fix up "
                                      "either 'A' or 'B' (or a symbol in one "
                                      "of their alt-entry groups)");
    }

    return PairRelocInfo(DeltaKind, TargetSymbol, Addend);
  }

  Error addRelocations() override {
    using namespace support;
    auto &Obj = getObject();

    for (auto &S : Obj.sections()) {

      JITTargetAddress SectionAddress = S.getAddress();

      for (auto RelItr = S.relocation_begin(), RelEnd = S.relocation_end();
           RelItr != RelEnd; ++RelItr) {

        MachO::relocation_info RI = getRelocationInfo(RelItr);

        // Sanity check the relocation kind.
        auto Kind = getRelocationKind(RI);
        if (!Kind)
          return Kind.takeError();

        // Find the address of the value to fix up.
        JITTargetAddress FixupAddress = SectionAddress + (uint32_t)RI.r_address;

        LLVM_DEBUG({
          dbgs() << "Processing " << getMachOARM64RelocationKindName(*Kind)
                 << " relocation at " << format("0x%016" PRIx64, FixupAddress)
                 << "\n";
        });

        // Find the block that the fixup points to.
        Block *BlockToFix = nullptr;
        {
          auto SymbolToFixOrErr = findSymbolByAddress(FixupAddress);
          if (!SymbolToFixOrErr)
            return SymbolToFixOrErr.takeError();
          BlockToFix = &SymbolToFixOrErr->getBlock();
        }

        if (FixupAddress + static_cast<JITTargetAddress>(1ULL << RI.r_length) >
            BlockToFix->getAddress() + BlockToFix->getContent().size())
          return make_error<JITLinkError>(
              "Relocation content extends past end of fixup block");

        // Get a pointer to the fixup content.
        const char *FixupContent = BlockToFix->getContent().data() +
                                   (FixupAddress - BlockToFix->getAddress());

        // The target symbol and addend will be populated by the switch below.
        Symbol *TargetSymbol = nullptr;
        uint64_t Addend = 0;

        if (*Kind == PairedAddend) {
          // If this is an Addend relocation then process it and move to the
          // paired reloc.

          Addend = RI.r_symbolnum;

          if (RelItr == RelEnd)
            return make_error<JITLinkError>("Unpaired Addend reloc at " +
                                            formatv("{0:x16}", FixupAddress));
          ++RelItr;
          RI = getRelocationInfo(RelItr);

          Kind = getRelocationKind(RI);
          if (!Kind)
            return Kind.takeError();

          if (*Kind != Branch26 && *Kind != Page21 && *Kind != PageOffset12)
            return make_error<JITLinkError>(
                "Invalid relocation pair: Addend + " +
                getMachOARM64RelocationKindName(*Kind));
          else
            LLVM_DEBUG({
              dbgs() << "  pair is " << getMachOARM64RelocationKindName(*Kind)
                     << "`\n";
            });

          // Find the address of the value to fix up.
          JITTargetAddress PairedFixupAddress =
              SectionAddress + (uint32_t)RI.r_address;
          if (PairedFixupAddress != FixupAddress)
            return make_error<JITLinkError>("Paired relocation points at "
                                            "different target");
        }

        switch (*Kind) {
        case Branch26: {
          if (auto TargetSymbolOrErr = findSymbolByIndex(RI.r_symbolnum))
            TargetSymbol = TargetSymbolOrErr->GraphSymbol;
          else
            return TargetSymbolOrErr.takeError();
          uint32_t Instr = *(const ulittle32_t *)FixupContent;
          if ((Instr & 0x7fffffff) != 0x14000000)
            return make_error<JITLinkError>("BRANCH26 target is not a B or BL "
                                            "instruction with a zero addend");
          break;
        }
        case Pointer32:
          if (auto TargetSymbolOrErr = findSymbolByIndex(RI.r_symbolnum))
            TargetSymbol = TargetSymbolOrErr->GraphSymbol;
          else
            return TargetSymbolOrErr.takeError();
          Addend = *(const ulittle32_t *)FixupContent;
          break;
        case Pointer64:
          if (auto TargetSymbolOrErr = findSymbolByIndex(RI.r_symbolnum))
            TargetSymbol = TargetSymbolOrErr->GraphSymbol;
          else
            return TargetSymbolOrErr.takeError();
          Addend = *(const ulittle64_t *)FixupContent;
          break;
        case Pointer64Anon: {
          JITTargetAddress TargetAddress = *(const ulittle64_t *)FixupContent;
          if (auto TargetSymbolOrErr = findSymbolByAddress(TargetAddress))
            TargetSymbol = &*TargetSymbolOrErr;
          else
            return TargetSymbolOrErr.takeError();
          Addend = TargetAddress - TargetSymbol->getAddress();
          break;
        }
        case Page21:
        case GOTPage21: {
          if (auto TargetSymbolOrErr = findSymbolByIndex(RI.r_symbolnum))
            TargetSymbol = TargetSymbolOrErr->GraphSymbol;
          else
            return TargetSymbolOrErr.takeError();
          uint32_t Instr = *(const ulittle32_t *)FixupContent;
          if ((Instr & 0xffffffe0) != 0x90000000)
            return make_error<JITLinkError>("PAGE21/GOTPAGE21 target is not an "
                                            "ADRP instruction with a zero "
                                            "addend");
          break;
        }
        case PageOffset12: {
          if (auto TargetSymbolOrErr = findSymbolByIndex(RI.r_symbolnum))
            TargetSymbol = TargetSymbolOrErr->GraphSymbol;
          else
            return TargetSymbolOrErr.takeError();
          break;
        }
        case GOTPageOffset12: {
          if (auto TargetSymbolOrErr = findSymbolByIndex(RI.r_symbolnum))
            TargetSymbol = TargetSymbolOrErr->GraphSymbol;
          else
            return TargetSymbolOrErr.takeError();
          uint32_t Instr = *(const ulittle32_t *)FixupContent;
          if ((Instr & 0xfffffc00) != 0xf9400000)
            return make_error<JITLinkError>("GOTPAGEOFF12 target is not an LDR "
                                            "immediate instruction with a zero "
                                            "addend");
          break;
        }
        case PointerToGOT:
          if (auto TargetSymbolOrErr = findSymbolByIndex(RI.r_symbolnum))
            TargetSymbol = TargetSymbolOrErr->GraphSymbol;
          else
            return TargetSymbolOrErr.takeError();
          break;
        case Delta32:
        case Delta64: {
          // We use Delta32/Delta64 to represent SUBTRACTOR relocations.
          // parsePairRelocation handles the paired reloc, and returns the
          // edge kind to be used (either Delta32/Delta64, or
          // NegDelta32/NegDelta64, depending on the direction of the
          // subtraction) along with the addend.
          auto PairInfo =
              parsePairRelocation(*BlockToFix, *Kind, RI, FixupAddress,
                                  FixupContent, ++RelItr, RelEnd);
          if (!PairInfo)
            return PairInfo.takeError();
          std::tie(*Kind, TargetSymbol, Addend) = *PairInfo;
          assert(TargetSymbol && "No target symbol from parsePairRelocation?");
          break;
        }
        default:
          llvm_unreachable("Special relocation kind should not appear in "
                           "mach-o file");
        }

        LLVM_DEBUG({
          Edge GE(*Kind, FixupAddress - BlockToFix->getAddress(), *TargetSymbol,
                  Addend);
          printEdge(dbgs(), *BlockToFix, GE,
                    getMachOARM64RelocationKindName(*Kind));
          dbgs() << "\n";
        });
        BlockToFix->addEdge(*Kind, FixupAddress - BlockToFix->getAddress(),
                            *TargetSymbol, Addend);
      }
    }
    return Error::success();
  }

  unsigned NumSymbols = 0;
};

class MachO_arm64_GOTAndStubsBuilder
    : public BasicGOTAndStubsBuilder<MachO_arm64_GOTAndStubsBuilder> {
public:
  MachO_arm64_GOTAndStubsBuilder(LinkGraph &G)
      : BasicGOTAndStubsBuilder<MachO_arm64_GOTAndStubsBuilder>(G) {}

  bool isGOTEdge(Edge &E) const {
    return E.getKind() == GOTPage21 || E.getKind() == GOTPageOffset12 ||
           E.getKind() == PointerToGOT;
  }

  Symbol &createGOTEntry(Symbol &Target) {
    auto &GOTEntryBlock = G.createContentBlock(
        getGOTSection(), getGOTEntryBlockContent(), 0, 8, 0);
    GOTEntryBlock.addEdge(Pointer64, 0, Target, 0);
    return G.addAnonymousSymbol(GOTEntryBlock, 0, 8, false, false);
  }

  void fixGOTEdge(Edge &E, Symbol &GOTEntry) {
    if (E.getKind() == GOTPage21 || E.getKind() == GOTPageOffset12) {
      // Update the target, but leave the edge addend as-is.
      E.setTarget(GOTEntry);
    } else if (E.getKind() == PointerToGOT) {
      E.setTarget(GOTEntry);
      E.setKind(Delta32);
    } else
      llvm_unreachable("Not a GOT edge?");
  }

  bool isExternalBranchEdge(Edge &E) {
    return E.getKind() == Branch26 && !E.getTarget().isDefined();
  }

  Symbol &createStub(Symbol &Target) {
    auto &StubContentBlock =
        G.createContentBlock(getStubsSection(), getStubBlockContent(), 0, 1, 0);
    // Re-use GOT entries for stub targets.
    auto &GOTEntrySymbol = getGOTEntrySymbol(Target);
    StubContentBlock.addEdge(LDRLiteral19, 0, GOTEntrySymbol, 0);
    return G.addAnonymousSymbol(StubContentBlock, 0, 8, true, false);
  }

  void fixExternalBranchEdge(Edge &E, Symbol &Stub) {
    assert(E.getKind() == Branch26 && "Not a Branch32 edge?");
    assert(E.getAddend() == 0 && "Branch32 edge has non-zero addend?");
    E.setTarget(Stub);
  }

private:
  Section &getGOTSection() {
    if (!GOTSection)
      GOTSection = &G.createSection("$__GOT", sys::Memory::MF_READ);
    return *GOTSection;
  }

  Section &getStubsSection() {
    if (!StubsSection) {
      auto StubsProt = static_cast<sys::Memory::ProtectionFlags>(
          sys::Memory::MF_READ | sys::Memory::MF_EXEC);
      StubsSection = &G.createSection("$__STUBS", StubsProt);
    }
    return *StubsSection;
  }

  StringRef getGOTEntryBlockContent() {
    return StringRef(reinterpret_cast<const char *>(NullGOTEntryContent),
                     sizeof(NullGOTEntryContent));
  }

  StringRef getStubBlockContent() {
    return StringRef(reinterpret_cast<const char *>(StubContent),
                     sizeof(StubContent));
  }

  static const uint8_t NullGOTEntryContent[8];
  static const uint8_t StubContent[8];
  Section *GOTSection = nullptr;
  Section *StubsSection = nullptr;
};

const uint8_t MachO_arm64_GOTAndStubsBuilder::NullGOTEntryContent[8] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t MachO_arm64_GOTAndStubsBuilder::StubContent[8] = {
    0x10, 0x00, 0x00, 0x58, // LDR x16, <literal>
    0x00, 0x02, 0x1f, 0xd6  // BR  x16
};

} // namespace

namespace llvm {
namespace jitlink {

class MachOJITLinker_arm64 : public JITLinker<MachOJITLinker_arm64> {
  friend class JITLinker<MachOJITLinker_arm64>;

public:
  MachOJITLinker_arm64(std::unique_ptr<JITLinkContext> Ctx,
                       PassConfiguration PassConfig)
      : JITLinker(std::move(Ctx), std::move(PassConfig)) {}

private:
  StringRef getEdgeKindName(Edge::Kind R) const override {
    return getMachOARM64RelocationKindName(R);
  }

  Expected<std::unique_ptr<LinkGraph>>
  buildGraph(MemoryBufferRef ObjBuffer) override {
    auto MachOObj = object::ObjectFile::createMachOObjectFile(ObjBuffer);
    if (!MachOObj)
      return MachOObj.takeError();
    return MachOLinkGraphBuilder_arm64(**MachOObj).buildGraph();
  }

  static Error targetOutOfRangeError(const Block &B, const Edge &E) {
    std::string ErrMsg;
    {
      raw_string_ostream ErrStream(ErrMsg);
      ErrStream << "Relocation target out of range: ";
      printEdge(ErrStream, B, E, getMachOARM64RelocationKindName(E.getKind()));
      ErrStream << "\n";
    }
    return make_error<JITLinkError>(std::move(ErrMsg));
  }

  static unsigned getPageOffset12Shift(uint32_t Instr) {
    constexpr uint32_t LDRLiteralMask = 0x3ffffc00;

    // Check for a GPR LDR immediate with a zero embedded literal.
    // If found, the top two bits contain the shift.
    if ((Instr & LDRLiteralMask) == 0x39400000)
      return Instr >> 30;

    // Check for a Neon LDR immediate of size 64-bit or less with a zero
    // embedded literal. If found, the top two bits contain the shift.
    if ((Instr & LDRLiteralMask) == 0x3d400000)
      return Instr >> 30;

    // Check for a Neon LDR immediate of size 128-bit with a zero embedded
    // literal.
    constexpr uint32_t SizeBitsMask = 0xc0000000;
    if ((Instr & (LDRLiteralMask | SizeBitsMask)) == 0x3dc00000)
      return 4;

    return 0;
  }

  Error applyFixup(Block &B, const Edge &E, char *BlockWorkingMem) const {
    using namespace support;

    char *FixupPtr = BlockWorkingMem + E.getOffset();
    JITTargetAddress FixupAddress = B.getAddress() + E.getOffset();

    switch (E.getKind()) {
    case Branch26: {
      assert((FixupAddress & 0x3) == 0 && "Branch-inst is not 32-bit aligned");

      int64_t Value = E.getTarget().getAddress() - FixupAddress + E.getAddend();

      if (static_cast<uint64_t>(Value) & 0x3)
        return make_error<JITLinkError>("Branch26 target is not 32-bit "
                                        "aligned");

      if (Value < -(1 << 27) || Value > ((1 << 27) - 1))
        return targetOutOfRangeError(B, E);

      uint32_t RawInstr = *(little32_t *)FixupPtr;
      assert((RawInstr & 0x7fffffff) == 0x14000000 &&
             "RawInstr isn't a B or BR immediate instruction");
      uint32_t Imm = (static_cast<uint32_t>(Value) & ((1 << 28) - 1)) >> 2;
      uint32_t FixedInstr = RawInstr | Imm;
      *(little32_t *)FixupPtr = FixedInstr;
      break;
    }
    case Pointer32: {
      uint64_t Value = E.getTarget().getAddress() + E.getAddend();
      if (Value > std::numeric_limits<uint32_t>::max())
        return targetOutOfRangeError(B, E);
      *(ulittle32_t *)FixupPtr = Value;
      break;
    }
    case Pointer64: {
      uint64_t Value = E.getTarget().getAddress() + E.getAddend();
      *(ulittle64_t *)FixupPtr = Value;
      break;
    }
    case Page21:
    case GOTPage21: {
      assert(E.getAddend() == 0 && "PAGE21/GOTPAGE21 with non-zero addend");
      uint64_t TargetPage =
          E.getTarget().getAddress() & ~static_cast<uint64_t>(4096 - 1);
      uint64_t PCPage = B.getAddress() & ~static_cast<uint64_t>(4096 - 1);

      int64_t PageDelta = TargetPage - PCPage;
      if (PageDelta < -(1 << 30) || PageDelta > ((1 << 30) - 1))
        return targetOutOfRangeError(B, E);

      uint32_t RawInstr = *(ulittle32_t *)FixupPtr;
      assert((RawInstr & 0xffffffe0) == 0x90000000 &&
             "RawInstr isn't an ADRP instruction");
      uint32_t ImmLo = (static_cast<uint64_t>(PageDelta) >> 12) & 0x3;
      uint32_t ImmHi = (static_cast<uint64_t>(PageDelta) >> 14) & 0x7ffff;
      uint32_t FixedInstr = RawInstr | (ImmLo << 29) | (ImmHi << 5);
      *(ulittle32_t *)FixupPtr = FixedInstr;
      break;
    }
    case PageOffset12: {
      assert(E.getAddend() == 0 && "PAGEOFF12 with non-zero addend");
      uint64_t TargetOffset = E.getTarget().getAddress() & 0xfff;

      uint32_t RawInstr = *(ulittle32_t *)FixupPtr;
      unsigned ImmShift = getPageOffset12Shift(RawInstr);

      if (TargetOffset & ((1 << ImmShift) - 1))
        return make_error<JITLinkError>("PAGEOFF12 target is not aligned");

      uint32_t EncodedImm = (TargetOffset >> ImmShift) << 10;
      uint32_t FixedInstr = RawInstr | EncodedImm;
      *(ulittle32_t *)FixupPtr = FixedInstr;
      break;
    }
    case GOTPageOffset12: {
      assert(E.getAddend() == 0 && "GOTPAGEOF12 with non-zero addend");
      uint64_t TargetOffset = E.getTarget().getAddress() & 0xfff;

      uint32_t RawInstr = *(ulittle32_t *)FixupPtr;
      assert((RawInstr & 0xfffffc00) == 0xf9400000 &&
             "RawInstr isn't a 64-bit LDR immediate");
      uint32_t FixedInstr = RawInstr | (TargetOffset << 10);
      *(ulittle32_t *)FixupPtr = FixedInstr;
      break;
    }
    case LDRLiteral19: {
      assert((FixupAddress & 0x3) == 0 && "LDR is not 32-bit aligned");
      assert(E.getAddend() == 0 && "LDRLiteral19 with non-zero addend");
      uint32_t RawInstr = *(ulittle32_t *)FixupPtr;
      assert(RawInstr == 0x58000010 && "RawInstr isn't a 64-bit LDR literal");
      int64_t Delta = E.getTarget().getAddress() - FixupAddress;
      if (Delta & 0x3)
        return make_error<JITLinkError>("LDR literal target is not 32-bit "
                                        "aligned");
      if (Delta < -(1 << 20) || Delta > ((1 << 20) - 1))
        return targetOutOfRangeError(B, E);

      uint32_t EncodedImm = (static_cast<uint32_t>(Delta) >> 2) << 5;
      uint32_t FixedInstr = RawInstr | EncodedImm;
      *(ulittle32_t *)FixupPtr = FixedInstr;
      break;
    }
    case Delta32:
    case Delta64:
    case NegDelta32:
    case NegDelta64: {
      int64_t Value;
      if (E.getKind() == Delta32 || E.getKind() == Delta64)
        Value = E.getTarget().getAddress() - FixupAddress + E.getAddend();
      else
        Value = FixupAddress - E.getTarget().getAddress() + E.getAddend();

      if (E.getKind() == Delta32 || E.getKind() == NegDelta32) {
        if (Value < std::numeric_limits<int32_t>::min() ||
            Value > std::numeric_limits<int32_t>::max())
          return targetOutOfRangeError(B, E);
        *(little32_t *)FixupPtr = Value;
      } else
        *(little64_t *)FixupPtr = Value;
      break;
    }
    default:
      llvm_unreachable("Unrecognized edge kind");
    }

    return Error::success();
  }

  uint64_t NullValue = 0;
};

void jitLink_MachO_arm64(std::unique_ptr<JITLinkContext> Ctx) {
  PassConfiguration Config;
  Triple TT("arm64-apple-ios");

  if (Ctx->shouldAddDefaultTargetPasses(TT)) {
    // Add a mark-live pass.
    if (auto MarkLive = Ctx->getMarkLivePass(TT))
      Config.PrePrunePasses.push_back(std::move(MarkLive));
    else
      Config.PrePrunePasses.push_back(markAllSymbolsLive);

    // Add an in-place GOT/Stubs pass.
    Config.PostPrunePasses.push_back([](LinkGraph &G) -> Error {
      MachO_arm64_GOTAndStubsBuilder(G).run();
      return Error::success();
    });
  }

  if (auto Err = Ctx->modifyPassConfig(TT, Config))
    return Ctx->notifyFailed(std::move(Err));

  // Construct a JITLinker and run the link function.
  MachOJITLinker_arm64::link(std::move(Ctx), std::move(Config));
}

StringRef getMachOARM64RelocationKindName(Edge::Kind R) {
  switch (R) {
  case Branch26:
    return "Branch26";
  case Pointer64:
    return "Pointer64";
  case Pointer64Anon:
    return "Pointer64Anon";
  case Page21:
    return "Page21";
  case PageOffset12:
    return "PageOffset12";
  case GOTPage21:
    return "GOTPage21";
  case GOTPageOffset12:
    return "GOTPageOffset12";
  case PointerToGOT:
    return "PointerToGOT";
  case PairedAddend:
    return "PairedAddend";
  case LDRLiteral19:
    return "LDRLiteral19";
  case Delta32:
    return "Delta32";
  case Delta64:
    return "Delta64";
  case NegDelta32:
    return "NegDelta32";
  case NegDelta64:
    return "NegDelta64";
  default:
    return getGenericEdgeKindName(static_cast<Edge::Kind>(R));
  }
}

} // end namespace jitlink
} // end namespace llvm
