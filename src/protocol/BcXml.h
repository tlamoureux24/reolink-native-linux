#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

// The small XML documents Baichuan replies carry, read as a tree, and edited in
// place for the "get, modify, set" writes the official SDK and reolink_aio do.
namespace rl::bcxml {

struct Node {
    QString name;
    QString text;
    QList<Node> kids;

    const Node *child(QStringView n) const;
    // Depth-first: the first element called `n` under this one.
    const Node *find(QStringView n) const;
    QList<const Node *> children(QStringView n) const;
    QString value(QStringView n) const; // trimmed text of child `n`
    int intValue(QStringView n, int dflt = 0) const;
};

Node parse(const QByteArray &xml);

// The text of the first <tag>…</tag> replaced by `value` (escaped); false if
// there is no such element.
bool setTag(QByteArray &xml, const char *tag, const QString &value);

QByteArray escaped(const QString &text);

} // namespace rl::bcxml
