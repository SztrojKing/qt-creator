/****************************************************************************
**
** Copyright (C) 2019 Denis Shienkov <denis.shienkov@gmail.com>
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

#include "baremetalconstants.h"
#include "keiltoolchain.h"

#include <projectexplorer/abiwidget.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/projectmacro.h>
#include <projectexplorer/toolchainmanager.h>

#include <utils/algorithm.h>
#include <utils/environment.h>
#include <utils/pathchooser.h>
#include <utils/qtcassert.h>
#include <utils/synchronousprocess.h>

#include <QDebug>
#include <QFileInfo>
#include <QFormLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTemporaryFile>
#include <QTextStream>

using namespace ProjectExplorer;
using namespace Utils;

namespace BareMetal {
namespace Internal {

// Helpers:

static const char compilerCommandKeyC[] = "BareMetal.KeilToolchain.CompilerPath";
static const char targetAbiKeyC[] = "BareMetal.KeilToolchain.TargetAbi";

static bool isCompilerExists(const FileName &compilerPath)
{
    const QFileInfo fi = compilerPath.toFileInfo();
    return fi.exists() && fi.isExecutable() && fi.isFile();
}

// Note: The KEIL 8051 compiler does not support the predefined
// macros dumping. So, we do it with following trick where we try
// to compile a temporary file and to parse the console output.
static Macros dumpC51PredefinedMacros(const FileName &compiler, const QStringList &env)
{
    QTemporaryFile fakeIn;
    if (!fakeIn.open())
        return {};
    fakeIn.write("#define VALUE_TO_STRING(x) #x\n");
    fakeIn.write("#define VALUE(x) VALUE_TO_STRING(x)\n");
    fakeIn.write("#define VAR_NAME_VALUE(var) \"\"\"|\"#var\"|\"VALUE(var)\n");
    fakeIn.write("#ifdef __C51__\n");
    fakeIn.write("#pragma message(VAR_NAME_VALUE(__C51__))\n");
    fakeIn.write("#endif\n");
    fakeIn.write("#ifdef __CX51__\n");
    fakeIn.write("#pragma message(VAR_NAME_VALUE(__CX51__))\n");
    fakeIn.write("#endif\n");
    fakeIn.close();

    SynchronousProcess cpp;
    cpp.setEnvironment(env);
    cpp.setTimeoutS(10);

    QStringList arguments;
    arguments.push_back(fakeIn.fileName());

    const SynchronousProcessResponse response = cpp.runBlocking(compiler.toString(), arguments);
    if (response.result != SynchronousProcessResponse::Finished
            || response.exitCode != 0) {
        qWarning() << response.exitMessage(compiler.toString(), 10);
        return {};
    }

    QString output = response.allOutput();
    Macros macros;
    QTextStream stream(&output);
    QString line;
    while (stream.readLineInto(&line)) {
        const QStringList parts = line.split("\"|\"");
        if (parts.count() != 3)
            continue;
        macros.push_back({parts.at(1).toUtf8(), parts.at(2).toUtf8()});
    }
    return macros;
}

static Macros dumpArmPredefinedMacros(const FileName &compiler, const QStringList &env)
{
    SynchronousProcess cpp;
    cpp.setEnvironment(env);
    cpp.setTimeoutS(10);

    QStringList arguments;
    arguments.push_back("-E");
    arguments.push_back("--list-macros");

    const SynchronousProcessResponse response = cpp.runBlocking(compiler.toString(), arguments);
    if (response.result != SynchronousProcessResponse::Finished
            || response.exitCode != 0) {
        qWarning() << response.exitMessage(compiler.toString(), 10);
        return {};
    }

    const QByteArray output = response.allOutput().toUtf8();
    return Macro::toMacros(output);
}

static Macros dumpPredefinedMacros(const FileName &compiler, const QStringList &env)
{
    if (compiler.isEmpty() || !compiler.toFileInfo().isExecutable())
        return {};

    const QFileInfo fi(compiler.toString());
    const QString bn = fi.baseName().toLower();
    // Check for C51 compiler first.
    if (bn.contains("c51") || bn.contains("cx51"))
        return dumpC51PredefinedMacros(compiler, env);

    return dumpArmPredefinedMacros(compiler, env);
}

static Abi::Architecture guessArchitecture(const Macros &macros)
{
    for (const Macro &macro : macros) {
        if (macro.key == "__CC_ARM")
            return Abi::Architecture::ArmArchitecture;
        if (macro.key == "__C51__" || macro.key == "__CX51__")
            return Abi::Architecture::Mcs51Architecture;
    }
    return Abi::Architecture::UnknownArchitecture;
}

static unsigned char guessWordWidth(const Macros &macros, Abi::Architecture arch)
{
    // Check for C51 compiler first.
    if (arch == Abi::Architecture::Mcs51Architecture)
        return 16; // C51 always have 16-bit word width.

    const Macro sizeMacro = Utils::findOrDefault(macros, [](const Macro &m) {
        return m.key == "__sizeof_int";
    });
    if (sizeMacro.isValid() && sizeMacro.type == MacroType::Define)
        return sizeMacro.value.toInt() * 8;
    return 0;
}

static Abi::BinaryFormat guessFormat(Abi::Architecture arch)
{
    if (arch == Abi::Architecture::ArmArchitecture)
        return Abi::BinaryFormat::ElfFormat;
    if (arch == Abi::Architecture::Mcs51Architecture)
        return Abi::BinaryFormat::OmfFormat;
    return Abi::BinaryFormat::UnknownFormat;
}

static Abi guessAbi(const Macros &macros)
{
    const auto arch = guessArchitecture(macros);
    return {arch, Abi::OS::BareMetalOS, Abi::OSFlavor::GenericFlavor,
                guessFormat(arch), guessWordWidth(macros, arch)};
}

// KeilToolchain

KeilToolchain::KeilToolchain(Detection d) :
    ToolChain(Constants::KEIL_TOOLCHAIN_TYPEID, d),
    m_predefinedMacrosCache(std::make_shared<Cache<MacroInspectionReport, 64>>())
{ }

KeilToolchain::KeilToolchain(Core::Id language, Detection d) :
    KeilToolchain(d)
{
    setLanguage(language);
}

QString KeilToolchain::typeDisplayName() const
{
    return Internal::KeilToolchainFactory::tr("KEIL");
}

void KeilToolchain::setTargetAbi(const Abi &abi)
{
    if (abi == m_targetAbi)
        return;
    m_targetAbi = abi;
    toolChainUpdated();
}

Abi KeilToolchain::targetAbi() const
{
    return m_targetAbi;
}

bool KeilToolchain::isValid() const
{
    return true;
}

ToolChain::MacroInspectionRunner KeilToolchain::createMacroInspectionRunner() const
{
    Environment env = Environment::systemEnvironment();
    addToEnvironment(env);

    const Utils::FileName compilerCommand = m_compilerCommand;
    const Core::Id lang = language();

    MacrosCache macroCache = m_predefinedMacrosCache;

    return [env, compilerCommand, macroCache, lang]
            (const QStringList &flags) {
        Q_UNUSED(flags)

        const Macros macros = dumpPredefinedMacros(compilerCommand, env.toStringList());
        const auto report = MacroInspectionReport{macros, languageVersion(lang, macros)};
        macroCache->insert({}, report);

        return report;
    };
}

Macros KeilToolchain::predefinedMacros(const QStringList &cxxflags) const
{
    return createMacroInspectionRunner()(cxxflags).macros;
}

Utils::LanguageExtensions KeilToolchain::languageExtensions(const QStringList &) const
{
    return LanguageExtension::None;
}

WarningFlags KeilToolchain::warningFlags(const QStringList &cxxflags) const
{
    Q_UNUSED(cxxflags);
    return WarningFlags::Default;
}

ToolChain::BuiltInHeaderPathsRunner KeilToolchain::createBuiltInHeaderPathsRunner() const
{
    return {};
}

HeaderPaths KeilToolchain::builtInHeaderPaths(const QStringList &cxxFlags,
                                              const FileName &fileName) const
{
    Q_UNUSED(cxxFlags)
    Q_UNUSED(fileName)
    return {};
}

void KeilToolchain::addToEnvironment(Environment &env) const
{
    if (!m_compilerCommand.isEmpty()) {
        const FileName path = m_compilerCommand.parentDir();
        env.prependOrSetPath(path.toString());
    }
}

IOutputParser *KeilToolchain::outputParser() const
{
    return nullptr;
}

QVariantMap KeilToolchain::toMap() const
{
    QVariantMap data = ToolChain::toMap();
    data.insert(compilerCommandKeyC, m_compilerCommand.toString());
    data.insert(targetAbiKeyC, m_targetAbi.toString());
    return data;
}

bool KeilToolchain::fromMap(const QVariantMap &data)
{
    if (!ToolChain::fromMap(data))
        return false;
    m_compilerCommand = FileName::fromString(data.value(compilerCommandKeyC).toString());
    m_targetAbi = Abi::fromString(data.value(targetAbiKeyC).toString());
    return true;
}

std::unique_ptr<ToolChainConfigWidget> KeilToolchain::createConfigurationWidget()
{
    return std::make_unique<KeilToolchainConfigWidget>(this);
}

bool KeilToolchain::operator ==(const ToolChain &other) const
{
    if (!ToolChain::operator ==(other))
        return false;

    const auto customTc = static_cast<const KeilToolchain *>(&other);
    return m_compilerCommand == customTc->m_compilerCommand
            && m_targetAbi == customTc->m_targetAbi
            ;
}

void KeilToolchain::setCompilerCommand(const FileName &file)
{
    if (file == m_compilerCommand)
        return;
    m_compilerCommand = file;
    toolChainUpdated();
}

FileName KeilToolchain::compilerCommand() const
{
    return m_compilerCommand;
}

QString KeilToolchain::makeCommand(const Environment &env) const
{
    Q_UNUSED(env)
    return {};
}

ToolChain *KeilToolchain::clone() const
{
    return new KeilToolchain(*this);
}

void KeilToolchain::toolChainUpdated()
{
    m_predefinedMacrosCache->invalidate();
    ToolChain::toolChainUpdated();
}

// KeilToolchainFactory

KeilToolchainFactory::KeilToolchainFactory()
{
    setDisplayName(tr("KEIL"));
}

QSet<Core::Id> KeilToolchainFactory::supportedLanguages() const
{
    return {ProjectExplorer::Constants::C_LANGUAGE_ID,
            ProjectExplorer::Constants::CXX_LANGUAGE_ID};
}

bool KeilToolchainFactory::canCreate()
{
    return true;
}

ToolChain *KeilToolchainFactory::create(Core::Id language)
{
    return new KeilToolchain(language, ToolChain::ManualDetection);
}

bool KeilToolchainFactory::canRestore(const QVariantMap &data)
{
    return typeIdFromMap(data) == Constants::KEIL_TOOLCHAIN_TYPEID;
}

ToolChain *KeilToolchainFactory::restore(const QVariantMap &data)
{
    const auto tc = new KeilToolchain(ToolChain::ManualDetection);
    if (tc->fromMap(data))
        return tc;

    delete tc;
    return nullptr;
}

// KeilToolchainConfigWidget

KeilToolchainConfigWidget::KeilToolchainConfigWidget(KeilToolchain *tc) :
    ToolChainConfigWidget(tc),
    m_compilerCommand(new PathChooser),
    m_abiWidget(new AbiWidget)
{
    m_compilerCommand->setExpectedKind(PathChooser::ExistingCommand);
    m_compilerCommand->setHistoryCompleter("PE.ToolChainCommand.History");
    m_mainLayout->addRow(tr("&Compiler path:"), m_compilerCommand);
    m_mainLayout->addRow(tr("&ABI:"), m_abiWidget);

    m_abiWidget->setEnabled(false);

    addErrorLabel();
    setFromToolchain();

    connect(m_compilerCommand, &PathChooser::rawPathChanged,
            this, &KeilToolchainConfigWidget::handleCompilerCommandChange);
    connect(m_abiWidget, &AbiWidget::abiChanged,
            this, &ToolChainConfigWidget::dirty);
}

void KeilToolchainConfigWidget::applyImpl()
{
    if (toolChain()->isAutoDetected())
        return;

    const auto tc = static_cast<KeilToolchain *>(toolChain());
    const QString displayName = tc->displayName();
    tc->setCompilerCommand(m_compilerCommand->fileName());
    tc->setTargetAbi(m_abiWidget->currentAbi());
    tc->setDisplayName(displayName);

    if (m_macros.isEmpty())
        return;

    const auto languageVersion = ToolChain::languageVersion(tc->language(), m_macros);
    tc->m_predefinedMacrosCache->insert({}, {m_macros, languageVersion});

    setFromToolchain();
}

bool KeilToolchainConfigWidget::isDirtyImpl() const
{
    const auto tc = static_cast<KeilToolchain *>(toolChain());
    return m_compilerCommand->fileName() != tc->compilerCommand()
            || m_abiWidget->currentAbi() != tc->targetAbi()
            ;
}

void KeilToolchainConfigWidget::makeReadOnlyImpl()
{
    m_mainLayout->setEnabled(false);
}

void KeilToolchainConfigWidget::setFromToolchain()
{
    const QSignalBlocker blocker(this);
    const auto tc = static_cast<KeilToolchain *>(toolChain());
    m_compilerCommand->setFileName(tc->compilerCommand());
    m_abiWidget->setAbis({}, tc->targetAbi());
    const bool haveCompiler = isCompilerExists(m_compilerCommand->fileName());
    m_abiWidget->setEnabled(haveCompiler);
}

void KeilToolchainConfigWidget::handleCompilerCommandChange()
{
    const FileName compilerPath = m_compilerCommand->fileName();
    const bool haveCompiler = isCompilerExists(compilerPath);
    if (haveCompiler) {
        const auto env = Environment::systemEnvironment();
        m_macros = dumpPredefinedMacros(compilerPath, env.toStringList());
        const Abi guessed = guessAbi(m_macros);
        m_abiWidget->setAbis({}, guessed);
    }

    m_abiWidget->setEnabled(haveCompiler);
    emit dirty();
}

} // namespace Internal
} // namespace BareMetal
