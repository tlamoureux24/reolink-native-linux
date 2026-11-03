#include "ReolinkApi.h"

#include <cmath>

namespace rl {
namespace api {

QVariant toVariant(const Json &j)
{
    switch (j.type()) {
    case Json::value_t::object: {
        QVariantMap m;
        for (auto it = j.begin(); it != j.end(); ++it)
            m.insert(QString::fromStdString(it.key()), toVariant(it.value()));
        return m;
    }
    case Json::value_t::array: {
        QVariantList l;
        for (const Json &e : j)
            l.append(toVariant(e));
        return l;
    }
    case Json::value_t::string:
        return QString::fromStdString(j.get<std::string>());
    case Json::value_t::boolean:
        return j.get<bool>();
    case Json::value_t::number_integer:
    case Json::value_t::number_unsigned:
        return static_cast<qlonglong>(j.get<qint64>());
    case Json::value_t::number_float:
        return j.get<double>();
    default:
        return {};
    }
}

Json toJson(const QVariant &v)
{
    switch (v.typeId()) {
    case QMetaType::QVariantMap: {
        Json o = Json::object();
        const QVariantMap m = v.toMap();
        for (auto it = m.begin(); it != m.end(); ++it)
            o[it.key().toStdString()] = toJson(it.value());
        return o;
    }
    case QMetaType::QVariantList: {
        Json a = Json::array();
        for (const QVariant &e : v.toList())
            a.push_back(toJson(e));
        return a;
    }
    case QMetaType::Bool:
        return v.toBool();
    case QMetaType::Int:
    case QMetaType::LongLong:
        return static_cast<qint64>(v.toLongLong());
    case QMetaType::Double:
    case QMetaType::Float: {
        // QML hands Math.round()/arithmetic results over as double; Reolink
        // firmware rejects 207.0 where it wants 207 ("param error").
        const double d = v.toDouble();
        if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 9.0e15)
            return static_cast<qint64>(d);
        return d;
    }
    case QMetaType::QString:
        return v.toString().toStdString();
    default:
        return v.isNull() ? Json() : Json(v.toString().toStdString());
    }
}

} // namespace api
} // namespace rl
