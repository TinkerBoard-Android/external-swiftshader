//===- subzero/src/IceELFObjectWriter.cpp - ELF object file writer --------===//
//
//                        The Subzero Code Generator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the writer for ELF relocatable object files.
//
//===----------------------------------------------------------------------===//

#include "assembler.h"
#include "IceDefs.h"
#include "IceELFObjectWriter.h"
#include "IceELFSection.h"
#include "IceELFStreamer.h"
#include "IceGlobalContext.h"
#include "IceGlobalInits.h"
#include "IceOperand.h"

using namespace llvm::ELF;

namespace Ice {

namespace {

struct {
  bool IsELF64;
  uint16_t ELFMachine;
  uint32_t ELFFlags;
} ELFTargetInfo[] = {
#define X(tag, str, is_elf64, e_machine, e_flags)                              \
  { is_elf64, e_machine, e_flags }                                             \
  ,
      TARGETARCH_TABLE
#undef X
};

bool isELF64(TargetArch Arch) {
  if (Arch < TargetArch_NUM)
    return ELFTargetInfo[Arch].IsELF64;
  llvm_unreachable("Invalid target arch for isELF64");
  return false;
}

uint16_t getELFMachine(TargetArch Arch) {
  if (Arch < TargetArch_NUM)
    return ELFTargetInfo[Arch].ELFMachine;
  llvm_unreachable("Invalid target arch for getELFMachine");
  return EM_NONE;
}

uint32_t getELFFlags(TargetArch Arch) {
  if (Arch < TargetArch_NUM)
    return ELFTargetInfo[Arch].ELFFlags;
  llvm_unreachable("Invalid target arch for getELFFlags");
  return 0;
}

} // end of anonymous namespace

ELFObjectWriter::ELFObjectWriter(GlobalContext &Ctx, ELFStreamer &Out)
    : Ctx(Ctx), Str(Out), SectionNumbersAssigned(false) {
  // Create the special bookkeeping sections now.
  const IceString NullSectionName("");
  NullSection = new (Ctx.allocate<ELFSection>())
      ELFSection(NullSectionName, SHT_NULL, 0, 0, 0);

  const IceString ShStrTabName(".shstrtab");
  ShStrTab = new (Ctx.allocate<ELFStringTableSection>())
      ELFStringTableSection(ShStrTabName, SHT_STRTAB, 0, 1, 0);
  ShStrTab->add(ShStrTabName);

  const IceString SymTabName(".symtab");
  bool IsELF64 = isELF64(Ctx.getTargetArch());
  const Elf64_Xword SymTabAlign = IsELF64 ? 8 : 4;
  const Elf64_Xword SymTabEntSize =
      IsELF64 ? sizeof(Elf64_Sym) : sizeof(Elf32_Sym);
  static_assert(sizeof(Elf64_Sym) == 24 && sizeof(Elf32_Sym) == 16,
                "Elf_Sym sizes cannot be derived from sizeof");
  SymTab = createSection<ELFSymbolTableSection>(SymTabName, SHT_SYMTAB, 0,
                                                SymTabAlign, SymTabEntSize);
  // The first entry in the symbol table should be a NULL entry.
  const IceString NullSymName("");
  SymTab->createDefinedSym(NullSymName, STT_NOTYPE, STB_LOCAL, NullSection, 0,
                           0);

  const IceString StrTabName(".strtab");
  StrTab =
      createSection<ELFStringTableSection>(StrTabName, SHT_STRTAB, 0, 1, 0);
}

template <typename T>
T *ELFObjectWriter::createSection(const IceString &Name, Elf64_Word ShType,
                                  Elf64_Xword ShFlags, Elf64_Xword ShAddralign,
                                  Elf64_Xword ShEntsize) {
  assert(!SectionNumbersAssigned);
  T *NewSection =
      new (Ctx.allocate<T>()) T(Name, ShType, ShFlags, ShAddralign, ShEntsize);
  ShStrTab->add(Name);
  return NewSection;
}

template <typename UserSectionList>
void ELFObjectWriter::assignRelSectionNumInPairs(SizeT &CurSectionNumber,
                                                 UserSectionList &UserSections,
                                                 RelSectionList &RelSections,
                                                 SectionList &AllSections) {
  RelSectionList::iterator RelIt = RelSections.begin();
  RelSectionList::iterator RelE = RelSections.end();
  for (ELFSection *UserSection : UserSections) {
    UserSection->setNumber(CurSectionNumber++);
    UserSection->setNameStrIndex(ShStrTab->getIndex(UserSection->getName()));
    AllSections.push_back(UserSection);
    if (RelIt != RelE) {
      ELFRelocationSection *RelSection = *RelIt;
      if (RelSection->getRelatedSection() == UserSection) {
        RelSection->setInfoNum(UserSection->getNumber());
        RelSection->setNumber(CurSectionNumber++);
        RelSection->setNameStrIndex(ShStrTab->getIndex(RelSection->getName()));
        AllSections.push_back(RelSection);
        ++RelIt;
      }
    }
  }
  // Should finish with UserIt at the same time as RelIt.
  assert(RelIt == RelE);
  return;
}

void ELFObjectWriter::assignRelLinkNum(SizeT SymTabNumber,
                                       RelSectionList &RelSections) {
  for (ELFRelocationSection *S : RelSections) {
    S->setLinkNum(SymTabNumber);
  }
}

void ELFObjectWriter::assignSectionNumbersInfo(SectionList &AllSections) {
  // Go through each section, assigning them section numbers and
  // and fill in the size for sections that aren't incrementally updated.
  assert(!SectionNumbersAssigned);
  SizeT CurSectionNumber = 0;
  NullSection->setNumber(CurSectionNumber++);
  // The rest of the fields are initialized to 0, and stay that way.
  AllSections.push_back(NullSection);

  assignRelSectionNumInPairs<TextSectionList>(CurSectionNumber, TextSections,
                                              RelTextSections, AllSections);
  assignRelSectionNumInPairs<DataSectionList>(CurSectionNumber, DataSections,
                                              RelDataSections, AllSections);
  assignRelSectionNumInPairs<DataSectionList>(CurSectionNumber, RoDataSections,
                                              RelRoDataSections, AllSections);

  ShStrTab->setNumber(CurSectionNumber++);
  ShStrTab->setNameStrIndex(ShStrTab->getIndex(ShStrTab->getName()));
  AllSections.push_back(ShStrTab);

  SymTab->setNumber(CurSectionNumber++);
  SymTab->setNameStrIndex(ShStrTab->getIndex(SymTab->getName()));
  AllSections.push_back(SymTab);

  StrTab->setNumber(CurSectionNumber++);
  StrTab->setNameStrIndex(ShStrTab->getIndex(StrTab->getName()));
  AllSections.push_back(StrTab);

  SymTab->setLinkNum(StrTab->getNumber());
  SymTab->setInfoNum(SymTab->getNumLocals());

  assignRelLinkNum(SymTab->getNumber(), RelTextSections);
  assignRelLinkNum(SymTab->getNumber(), RelDataSections);
  assignRelLinkNum(SymTab->getNumber(), RelRoDataSections);
  SectionNumbersAssigned = true;
}

Elf64_Off ELFObjectWriter::alignFileOffset(Elf64_Xword Align) {
  assert(llvm::isPowerOf2_32(Align));
  Elf64_Off OffsetInFile = Str.tell();
  Elf64_Xword Mod = OffsetInFile & (Align - 1);
  if (Mod == 0)
    return OffsetInFile;
  Elf64_Xword AlignDiff = Align - Mod;
  Str.writeZeroPadding(AlignDiff);
  OffsetInFile += AlignDiff;
  assert((OffsetInFile & (Align - 1)) == 0);
  return OffsetInFile;
}

void ELFObjectWriter::writeFunctionCode(const IceString &FuncName,
                                        bool IsInternal, const Assembler *Asm) {
  assert(!SectionNumbersAssigned);
  ELFTextSection *Section = nullptr;
  // TODO(jvoung): handle ffunction-sections.
  IceString SectionName = ".text";
  if (TextSections.size() == 0) {
    const Elf64_Xword ShFlags = SHF_ALLOC | SHF_EXECINSTR;
    // TODO(jvoung): Should be bundle size. Grab it from that target?
    const Elf64_Xword ShAlign = 32;
    Section = createSection<ELFTextSection>(SectionName, SHT_PROGBITS, ShFlags,
                                            ShAlign, 0);
    Elf64_Off OffsetInFile = alignFileOffset(Section->getSectionAlign());
    Section->setFileOffset(OffsetInFile);
    TextSections.push_back(Section);
  } else {
    Section = TextSections[0];
  }
  RelocOffsetT OffsetInSection = Section->getCurrentSize();
  // Function symbols are set to 0 size in the symbol table,
  // in contrast to data symbols which have a proper size.
  SizeT SymbolSize = 0;
  Section->appendData(Str, Asm->getBufferView());
  uint8_t SymbolType;
  uint8_t SymbolBinding;
  if (IsInternal) {
    SymbolType = STT_NOTYPE;
    SymbolBinding = STB_LOCAL;
  } else {
    SymbolType = STT_FUNC;
    SymbolBinding = STB_GLOBAL;
  }
  SymTab->createDefinedSym(FuncName, SymbolType, SymbolBinding, Section,
                           OffsetInSection, SymbolSize);
  StrTab->add(FuncName);

  // Create a relocation section for the text section if needed, and copy the
  // fixup information from per-function Assembler memory to the object
  // writer's memory, for writing later.
  if (!Asm->fixups().empty()) {
    bool IsELF64 = isELF64(Ctx.getTargetArch());
    IceString RelSectionName = IsELF64 ? ".rela" : ".rel";
    RelSectionName += SectionName;
    ELFRelocationSection *RelSection = nullptr;
    // TODO(jvoung): Make this more efficient if -ffunction-sections
    // efficiency becomes a problem.
    auto RSI =
        std::find_if(RelTextSections.begin(), RelTextSections.end(),
                     [&RelSectionName](const ELFRelocationSection *S)
                         -> bool { return S->getName() == RelSectionName; });
    if (RSI != RelTextSections.end()) {
      RelSection = *RSI;
    } else {
      const Elf64_Word ShType = IsELF64 ? SHT_RELA : SHT_REL;
      const Elf64_Xword ShAlign = IsELF64 ? 8 : 4;
      const Elf64_Xword ShEntSize =
          IsELF64 ? sizeof(Elf64_Rela) : sizeof(Elf32_Rel);
      static_assert(sizeof(Elf64_Rela) == 24 && sizeof(Elf32_Rel) == 8,
                    "Elf_Rel/Rela sizes cannot be derived from sizeof");
      const Elf64_Xword ShFlags = 0;
      RelSection = createSection<ELFRelocationSection>(
          RelSectionName, ShType, ShFlags, ShAlign, ShEntSize);
      RelSection->setRelatedSection(Section);
      RelTextSections.push_back(RelSection);
    }
    RelSection->addRelocations(OffsetInSection, Asm->fixups());
  }
}

void ELFObjectWriter::writeDataInitializer(const IceString &VarName,
                                           const llvm::StringRef Data) {
  assert(!SectionNumbersAssigned);
  (void)Data;
  llvm_unreachable("TODO");
  StrTab->add(VarName);
}

void ELFObjectWriter::writeInitialELFHeader() {
  assert(!SectionNumbersAssigned);
  const Elf64_Off DummySHOffset = 0;
  const SizeT DummySHStrIndex = 0;
  const SizeT DummyNumSections = 0;
  if (isELF64(Ctx.getTargetArch())) {
    writeELFHeaderInternal<true>(DummySHOffset, DummySHStrIndex,
                                 DummyNumSections);
  } else {
    writeELFHeaderInternal<false>(DummySHOffset, DummySHStrIndex,
                                  DummyNumSections);
  }
}

template <bool IsELF64>
void ELFObjectWriter::writeELFHeaderInternal(Elf64_Off SectionHeaderOffset,
                                             SizeT SectHeaderStrIndex,
                                             SizeT NumSections) {
  // Write the e_ident: magic number, class, etc.
  // The e_ident is byte order and ELF class independent.
  Str.writeBytes(llvm::StringRef(ElfMagic, strlen(ElfMagic)));
  Str.write8(IsELF64 ? ELFCLASS64 : ELFCLASS32);
  Str.write8(ELFDATA2LSB);
  Str.write8(EV_CURRENT);
  Str.write8(ELFOSABI_NONE);
  const uint8_t ELF_ABIVersion = 0;
  Str.write8(ELF_ABIVersion);
  Str.writeZeroPadding(EI_NIDENT - EI_PAD);

  // TODO(jvoung): Handle and test > 64K sections.  See the generic ABI doc:
  // https://refspecs.linuxbase.org/elf/gabi4+/ch4.eheader.html
  // e_shnum should be 0 and then actual number of sections is
  // stored in the sh_size member of the 0th section.
  assert(NumSections < SHN_LORESERVE);
  assert(SectHeaderStrIndex < SHN_LORESERVE);

  // Write the rest of the file header, which does depend on byte order
  // and ELF class.
  Str.writeLE16(ET_REL);                             // e_type
  Str.writeLE16(getELFMachine(Ctx.getTargetArch())); // e_machine
  Str.writeELFWord<IsELF64>(1);                      // e_version
  // Since this is for a relocatable object, there is no entry point,
  // and no program headers.
  Str.writeAddrOrOffset<IsELF64>(0);                                // e_entry
  Str.writeAddrOrOffset<IsELF64>(0);                                // e_phoff
  Str.writeAddrOrOffset<IsELF64>(SectionHeaderOffset);              // e_shoff
  Str.writeELFWord<IsELF64>(getELFFlags(Ctx.getTargetArch()));      // e_flags
  Str.writeLE16(IsELF64 ? sizeof(Elf64_Ehdr) : sizeof(Elf32_Ehdr)); // e_ehsize
  static_assert(sizeof(Elf64_Ehdr) == 64 && sizeof(Elf32_Ehdr) == 52,
                "Elf_Ehdr sizes cannot be derived from sizeof");
  Str.writeLE16(0); // e_phentsize
  Str.writeLE16(0); // e_phnum
  Str.writeLE16(IsELF64 ? sizeof(Elf64_Shdr)
                        : sizeof(Elf32_Shdr)); // e_shentsize
  static_assert(sizeof(Elf64_Shdr) == 64 && sizeof(Elf32_Shdr) == 40,
                "Elf_Shdr sizes cannot be derived from sizeof");
  Str.writeLE16(static_cast<Elf64_Half>(NumSections));        // e_shnum
  Str.writeLE16(static_cast<Elf64_Half>(SectHeaderStrIndex)); // e_shstrndx
}

template <typename ConstType> void ELFObjectWriter::writeConstantPool(Type Ty) {
  ConstantList Pool = Ctx.getConstantPool(Ty);
  if (Pool.empty()) {
    return;
  }
  SizeT Align = typeAlignInBytes(Ty);
  size_t EntSize = typeWidthInBytes(Ty);
  char Buf[20];
  SizeT WriteAmt = std::min(EntSize, llvm::array_lengthof(Buf));
  assert(WriteAmt == EntSize);
  // Assume that writing WriteAmt bytes at a time allows us to avoid aligning
  // between entries.
  assert(WriteAmt % Align == 0);
  // Check that we write the full PrimType.
  assert(WriteAmt == sizeof(typename ConstType::PrimType));
  const Elf64_Xword ShFlags = SHF_ALLOC | SHF_MERGE;
  std::string SecBuffer;
  llvm::raw_string_ostream SecStrBuf(SecBuffer);
  SecStrBuf << ".rodata.cst" << WriteAmt;
  ELFDataSection *Section = createSection<ELFDataSection>(
      SecStrBuf.str(), SHT_PROGBITS, ShFlags, Align, WriteAmt);
  RoDataSections.push_back(Section);
  SizeT OffsetInSection = 0;
  // The symbol table entry doesn't need to know the defined symbol's
  // size since this is in a section with a fixed Entry Size.
  const SizeT SymbolSize = 0;
  Section->setFileOffset(alignFileOffset(Align));

  // Write the data.
  for (Constant *C : Pool) {
    auto Const = llvm::cast<ConstType>(C);
    std::string SymBuffer;
    llvm::raw_string_ostream SymStrBuf(SymBuffer);
    Const->emitPoolLabel(SymStrBuf);
    std::string &SymName = SymStrBuf.str();
    SymTab->createDefinedSym(SymName, STT_NOTYPE, STB_LOCAL, Section,
                             OffsetInSection, SymbolSize);
    StrTab->add(SymName);
    typename ConstType::PrimType Value = Const->getValue();
    memcpy(Buf, &Value, WriteAmt);
    Str.writeBytes(llvm::StringRef(Buf, WriteAmt));
    OffsetInSection += WriteAmt;
  }
  Section->setSize(OffsetInSection);
}

// Instantiate known needed versions of the template, since we are
// defining the function in the .cpp file instead of the .h file.
// We may need to instantiate constant pools for integers as well
// if we do constant-pooling of large integers to remove them
// from the instruction stream (fewer bytes controlled by an attacker).
template void ELFObjectWriter::writeConstantPool<ConstantFloat>(Type Ty);

template void ELFObjectWriter::writeConstantPool<ConstantDouble>(Type Ty);

void ELFObjectWriter::writeAllRelocationSections(bool IsELF64) {
  writeRelocationSections(IsELF64, RelTextSections);
  writeRelocationSections(IsELF64, RelDataSections);
  writeRelocationSections(IsELF64, RelRoDataSections);
}

void ELFObjectWriter::writeRelocationSections(bool IsELF64,
                                              RelSectionList &RelSections) {
  for (ELFRelocationSection *RelSec : RelSections) {
    Elf64_Off Offset = alignFileOffset(RelSec->getSectionAlign());
    RelSec->setFileOffset(Offset);
    RelSec->setSize(RelSec->getSectionDataSize(Ctx, SymTab));
    if (IsELF64) {
      RelSec->writeData<true>(Ctx, Str, SymTab);
    } else {
      RelSec->writeData<false>(Ctx, Str, SymTab);
    }
  }
}

void ELFObjectWriter::writeNonUserSections() {
  bool IsELF64 = isELF64(Ctx.getTargetArch());

  // Write out the shstrtab now that all sections are known.
  ShStrTab->doLayout();
  ShStrTab->setSize(ShStrTab->getSectionDataSize());
  Elf64_Off ShStrTabOffset = alignFileOffset(ShStrTab->getSectionAlign());
  ShStrTab->setFileOffset(ShStrTabOffset);
  Str.writeBytes(ShStrTab->getSectionData());

  SectionList AllSections;
  assignSectionNumbersInfo(AllSections);

  // Finalize the regular StrTab and fix up references in the SymTab.
  StrTab->doLayout();
  StrTab->setSize(StrTab->getSectionDataSize());

  SymTab->updateIndices(StrTab);

  Elf64_Off SymTabOffset = alignFileOffset(SymTab->getSectionAlign());
  SymTab->setFileOffset(SymTabOffset);
  SymTab->setSize(SymTab->getSectionDataSize());
  SymTab->writeData(Str, IsELF64);

  Elf64_Off StrTabOffset = alignFileOffset(StrTab->getSectionAlign());
  StrTab->setFileOffset(StrTabOffset);
  Str.writeBytes(StrTab->getSectionData());

  writeAllRelocationSections(IsELF64);

  // Write out the section headers.
  const size_t ShdrAlign = IsELF64 ? 8 : 4;
  Elf64_Off ShOffset = alignFileOffset(ShdrAlign);
  for (const auto S : AllSections) {
    if (IsELF64)
      S->writeHeader<true>(Str);
    else
      S->writeHeader<false>(Str);
  }

  // Finally write the updated ELF header w/ the correct number of sections.
  Str.seek(0);
  if (IsELF64) {
    writeELFHeaderInternal<true>(ShOffset, ShStrTab->getNumber(),
                                 AllSections.size());
  } else {
    writeELFHeaderInternal<false>(ShOffset, ShStrTab->getNumber(),
                                  AllSections.size());
  }
}

} // end of namespace Ice
