#include "BcXml.h"

#include <QXmlStreamReader>

namespace rl::bcxml {

const Node *Node::child(QStringView n) const
{
    for (const Node &k : kids)
        if (k.name == n)
            return &k;
    return nullptr;
}

const Node *Node::find(QStringView n) const
{
    for (const Node &k : kids) {
        if (k.name == n)
            return &k;
        if (const Node *f = k.find(n))
            return f;
    }
    return nullptr;
}

QList<const Node *> Node::children(QStringView n) const
{
    QList<const Node *> out;
    for (const Node &k : kids)
        if (k.name == n)
            out.append(&k);
    return out;
}

QString Node::value(QStringView n) const
{
    const Node *c = child(n);
    return c ? c->text.trimmed() : QString();
}

int Node::intValue(QStringView n, int dflt) const
{
    bool ok = false;
    const int v = value(n).toInt(&ok);
    return ok ? v : dflt;
}

Node parse(const QByteArray &xml)
{
    Node root;
    QList<Node *> stack{&root};
    QXmlStreamReader r(xml);
    while (!r.atEnd()) {
        switch (r.readNext()) {
        case QXmlStreamReader::StartElement: {
            Node *parent = stack.last();
            parent->kids.append(Node{r.name().toString(), {}, {}});
            stack.append(&parent->kids.last());
            break;
        }
        case QXmlStreamReader::Characters:
            stack.last()->text += r.text();
            break;
        case QXmlStreamReader::EndElement:
            if (stack.size() > 1)
                stack.removeLast();
            break;
        default:
            break;
        }
    }
    return root;
}

QByteArray escaped(const QString &text)
{
    return text.toHtmlEscaped().toUtf8();
}

bool setTag(QByteArray &xml, const char *tag, const QString &value)
{
    const QByteArray open = '<' + QByteArray(tag) + '>';
    const QByteArray close = "</" + QByteArray(tag) + '>';
    const int a = xml.indexOf(open);
    if (a < 0)
        return false;
    const int z = xml.indexOf(close, a);
    if (z < 0)
        return false;
    xml.replace(a + open.size(), z - a - open.size(), escaped(value));
    return true;
}

} // namespace rl::bcxml
