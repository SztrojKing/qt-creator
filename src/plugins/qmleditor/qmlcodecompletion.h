#ifndef QMLCODECOMPLETION_H
#define QMLCODECOMPLETION_H

#include <qml/metatype/qmltypesystem.h>
#include <texteditor/icompletioncollector.h>

namespace TextEditor {
class ITextEditable;
}

namespace QmlEditor {

class QmlModelManagerInterface;

namespace Internal {

class QmlCodeCompletion: public TextEditor::ICompletionCollector
{
    Q_OBJECT

public:
    QmlCodeCompletion(QmlModelManagerInterface *modelManager, Qml::QmlTypeSystem *typeSystem, QObject *parent = 0);
    virtual ~QmlCodeCompletion();

    Qt::CaseSensitivity caseSensitivity() const;
    void setCaseSensitivity(Qt::CaseSensitivity caseSensitivity);

    virtual bool supportsEditor(TextEditor::ITextEditable *editor);
    virtual bool triggersCompletion(TextEditor::ITextEditable *editor);
    virtual int startCompletion(TextEditor::ITextEditable *editor);
    virtual void completions(QList<TextEditor::CompletionItem> *completions);
    virtual void complete(const TextEditor::CompletionItem &item);
    virtual bool partiallyComplete(const QList<TextEditor::CompletionItem> &completionItems);
    virtual void cleanup();

private:
    QmlModelManagerInterface *m_modelManager;
    TextEditor::ITextEditable *m_editor;
    int m_startPosition;
    QList<TextEditor::CompletionItem> m_completions;
    Qt::CaseSensitivity m_caseSensitivity;
    Qml::QmlTypeSystem *m_typeSystem;
};


} // end of namespace Internal
} // end of namespace QmlEditor

#endif // QMLCODECOMPLETION_H
