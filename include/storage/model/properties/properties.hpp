#pragma once

#include <unordered_set>

#include "utils/option.hpp"
#include "utils/option_ptr.hpp"

#include "storage/model/properties/property_family.hpp"
#include "storage/model/properties/stored_property.hpp"

// TG - type group (TypeGroupEntity)

// TODO: think about a better place
template <class TG, class T>
using type_key_t =
    typename PropertyFamily<TG>::PropertyType::template PropertyTypeKey<T>;

// Collection of stored properties.
// NOTE: Currently underlying strucutre is a vector which is fine for smaller
// number of properties.
template <class TG>
class Properties
{
public:
    using property_key =
        typename PropertyFamily<TG>::PropertyType::PropertyFamilyKey;

    template <class T>
    using type_key_t =
        typename PropertyFamily<TG>::PropertyType::template PropertyTypeKey<T>;

    auto begin() const { return props.begin(); }
    auto cbegin() const { return props.cbegin(); }

    auto end() const { return props.end(); }
    auto cend() const { return props.cend(); }

    size_t size() const { return props.size(); }

    const bool contains(property_key &key) const;

    const StoredProperty<TG> &at(PropertyFamily<TG> &key) const;

    const StoredProperty<TG> &at(property_key &key) const;

    template <class T>
    OptionPtr<const T> at(type_key_t<T> &key) const
    {
        for (auto &prop : props) {
            if (prop.key == key) {
                if (prop.template is<T>()) {
                    return OptionPtr<const T>(&(prop.template as<T>()));
                }
                break;
            }
        }

        return OptionPtr<const T>();
    }

    void set(StoredProperty<TG> &&value);

    void clear(property_key &key);

    void clear(PropertyFamily<TG> &key);

    template <class Handler>
    void accept(Handler &handler) const
    {
        for (auto &kv : props)
            kv.accept(handler);

        handler.finish();
    }

    template <class Handler>
    void handle(Handler &handler) const
    {
        for (auto &kv : props)
            handler.handle(kv);

        handler.finish();
    }

    template <class Handler>
    void for_all(Handler handler) const
    {
        for (auto &kv : props)
            handler(kv);
    }

private:
    using props_t = std::vector<StoredProperty<TG>>;
    // TODO: more bytes can be saved if this is array with exact size as number
    // of elements.
    // TODO: even more bytes can be saved if this is one ptr to structure which
    // holds len followed by len sized array.
    props_t props;
};
