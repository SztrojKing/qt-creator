/****************************************************************************
**
** Copyright (C) 2017 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#pragma once

#include "symbolsvisitorbase.h"
#include "sourcelocationsutils.h"
#include "sourcelocationentry.h"
#include "symbolentry.h"
#include "useddefines.h"

#include <filepath.h>
#include <filepathid.h>

#include <clang/Lex/MacroInfo.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>

namespace ClangBackEnd {

class CollectMacrosPreprocessorCallbacks final : public clang::PPCallbacks,
                                                 public SymbolsVisitorBase
{
public:
    CollectMacrosPreprocessorCallbacks(SymbolEntries &symbolEntries,
                                       SourceLocationEntries &sourceLocationEntries,
                                       FilePathIds &sourceFiles,
                                       UsedDefines &usedDefines,
                                       FilePathCachingInterface &filePathCache,
                                       const clang::SourceManager &sourceManager,
                                       std::shared_ptr<clang::Preprocessor> &&preprocessor)
        : SymbolsVisitorBase(filePathCache, sourceManager),
          m_preprocessor(std::move(preprocessor)),
          m_symbolEntries(symbolEntries),
          m_sourceLocationEntries(sourceLocationEntries),
          m_sourceFiles(sourceFiles),
          m_usedDefines(usedDefines)
    {
    }

    void InclusionDirective(clang::SourceLocation /*hashLocation*/,
                            const clang::Token &/*includeToken*/,
                            llvm::StringRef /*fileName*/,
                            bool /*isAngled*/,
                            clang::CharSourceRange /*fileNameRange*/,
                            const clang::FileEntry *file,
                            llvm::StringRef /*searchPath*/,
                            llvm::StringRef /*relativePath*/,
                            const clang::Module * /*imported*/) override
    {
        if (!m_skipInclude && file)
            addSourceFile(file);

        m_skipInclude = false;
    }

    bool FileNotFound(clang::StringRef /*fileNameRef*/, clang::SmallVectorImpl<char> &/*recoveryPath*/) override
    {
        m_skipInclude = true;

        return true;
    }

    void Ifndef(clang::SourceLocation,
                const clang::Token &macroNameToken,
                const clang::MacroDefinition &macroDefinition) override
    {
        addUsedDefine(macroNameToken, macroDefinition);
        addMacroAsSymbol(macroNameToken,
                         firstMacroInfo(macroDefinition.getLocalDirective()),
                         SymbolType::MacroUsage);
    }

    void Ifdef(clang::SourceLocation,
               const clang::Token &macroNameToken,
               const clang::MacroDefinition &macroDefinition) override
    {
        addUsedDefine( macroNameToken, macroDefinition);
        addMacroAsSymbol(macroNameToken,
                         firstMacroInfo(macroDefinition.getLocalDirective()),
                         SymbolType::MacroUsage);
    }

    void Defined(const clang::Token &macroNameToken,
                 const clang::MacroDefinition &macroDefinition,
                 clang::SourceRange) override
    {
        addUsedDefine(macroNameToken, macroDefinition);
        addMacroAsSymbol(macroNameToken,
                         firstMacroInfo(macroDefinition.getLocalDirective()),
                         SymbolType::MacroUsage);
    }

    void MacroDefined(const clang::Token &macroNameToken,
                      const clang::MacroDirective *macroDirective) override
    {
        addMacroAsSymbol(macroNameToken, firstMacroInfo(macroDirective), SymbolType::MacroDefinition);
    }

    void MacroUndefined(const clang::Token &macroNameToken,
                        const clang::MacroDefinition &macroDefinition,
                        const clang::MacroDirective *) override
    {
        addMacroAsSymbol(macroNameToken,
                         firstMacroInfo(macroDefinition.getLocalDirective()),
                         SymbolType::MacroUndefinition);
    }

    void MacroExpands(const clang::Token &macroNameToken,
                      const clang::MacroDefinition &macroDefinition,
                      clang::SourceRange,
                      const clang::MacroArgs *) override
    {
        addUsedDefine(macroNameToken, macroDefinition);
        addMacroAsSymbol(macroNameToken,
                         firstMacroInfo(macroDefinition.getLocalDirective()),
                         SymbolType::MacroUsage);
    }

    void EndOfMainFile() override
    {
        filterOutHeaderGuards();
        mergeUsedDefines();
        filterOutExports();
    }

    void filterOutHeaderGuards()
    {
        auto partitionPoint = std::stable_partition(m_maybeUsedDefines.begin(),
                                                    m_maybeUsedDefines.end(),
                                                    [&] (const UsedDefine &usedDefine) {
            llvm::StringRef id{usedDefine.defineName.data(), usedDefine.defineName.size()};
            clang::IdentifierInfo &identifierInfo = m_preprocessor->getIdentifierTable().get(id);
            clang::MacroInfo *macroInfo = m_preprocessor->getMacroInfo(&identifierInfo);
            return !macroInfo || !macroInfo->isUsedForHeaderGuard();
        });

        m_maybeUsedDefines.erase(partitionPoint, m_maybeUsedDefines.end());
    }

    void filterOutExports()
    {
        auto partitionPoint = std::stable_partition(m_usedDefines.begin(),
                                                    m_usedDefines.end(),
                                                    [&] (const UsedDefine &usedDefine) {
            return !usedDefine.defineName.contains("EXPORT");
        });

        m_usedDefines.erase(partitionPoint, m_usedDefines.end());
    }

    void mergeUsedDefines()
    {
        m_usedDefines.reserve(m_usedDefines.size() + m_maybeUsedDefines.size());
        auto insertionPoint = m_usedDefines.insert(m_usedDefines.end(),
                                                   m_maybeUsedDefines.begin(),
                                                   m_maybeUsedDefines.end());
        std::inplace_merge(m_usedDefines.begin(), insertionPoint, m_usedDefines.end());
    }

    static void addUsedDefine(UsedDefine &&usedDefine, UsedDefines &usedDefines)
    {
        auto found = std::lower_bound(usedDefines.begin(),
                                      usedDefines.end(), usedDefine);

        if (found == usedDefines.end() || *found != usedDefine)
            usedDefines.insert(found, std::move(usedDefine));
    }

    void addUsedDefine(const clang::Token &macroNameToken,
                       const clang::MacroDefinition &macroDefinition)
    {
        clang::MacroInfo *macroInfo = macroDefinition.getMacroInfo();
        UsedDefine usedDefine{macroNameToken.getIdentifierInfo()->getName(),
                              filePathId(macroNameToken.getLocation())};
        if (macroInfo)
            addUsedDefine(std::move(usedDefine), m_usedDefines);
        else
            addUsedDefine(std::move(usedDefine), m_maybeUsedDefines);
    }

    static const clang::MacroInfo *firstMacroInfo(const clang::MacroDirective *macroDirective)
    {
        if (macroDirective) {
            const clang::MacroDirective *previousDirective = macroDirective;
            do {
                macroDirective = previousDirective;
                previousDirective = macroDirective->getPrevious();
            } while (previousDirective);

            return macroDirective->getMacroInfo();
        }

        return nullptr;
    }

    void addMacroAsSymbol(const clang::Token &macroNameToken,
                          const clang::MacroInfo *macroInfo,
                          SymbolType symbolType)
    {
        clang::SourceLocation sourceLocation = macroNameToken.getLocation();
        if (macroInfo && sourceLocation.isFileID()) {
            FilePathId fileId = filePathId(sourceLocation);
            if (fileId.isValid()) {
                auto macroName = macroNameToken.getIdentifierInfo()->getName();
                SymbolIndex globalId = toSymbolIndex(macroInfo);

                auto found = m_symbolEntries.find(globalId);
                if (found == m_symbolEntries.end()) {
                    Utils::optional<Utils::PathString> usr = generateUSR(macroName, sourceLocation);
                    if (usr) {
                        m_symbolEntries.emplace(std::piecewise_construct,
                                                std::forward_as_tuple(globalId),
                                                std::forward_as_tuple(std::move(usr.value()), macroName));
                    }
                }

                m_sourceLocationEntries.emplace_back(globalId,
                                                     fileId,
                                                     lineColum(sourceLocation),
                                                     symbolType);
            }

        }
    }

    void addSourceFile(const clang::FileEntry *file)
    {
        auto filePathId = m_filePathCache.filePathId(
                    FilePath::fromNativeFilePath(absolutePath(file->getName())));

        auto found = std::find(m_sourceFiles.begin(), m_sourceFiles.end(), filePathId);

        if (found == m_sourceFiles.end() || *found != filePathId)
            m_sourceFiles.insert(found, filePathId);
    }

private:
    UsedDefines m_maybeUsedDefines;
    std::shared_ptr<clang::Preprocessor> m_preprocessor;
    SymbolEntries &m_symbolEntries;
    SourceLocationEntries &m_sourceLocationEntries;
    FilePathIds &m_sourceFiles;
    UsedDefines &m_usedDefines;
    bool m_skipInclude = false;
};

} // namespace ClangBackEnd
