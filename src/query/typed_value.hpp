#pragma once

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "query/path.hpp"
#include "storage/common/types/property_value.hpp"
#include "storage/edge_accessor.hpp"
#include "storage/vertex_accessor.hpp"
#include "utils/exceptions.hpp"
#include "utils/memory.hpp"

namespace query {

// TODO: Neo4j does overflow checking. Should we also implement it?
/**
 * Stores a query runtime value and its type.
 *
 * Values can be of a number of predefined types that are enumerated in
 * TypedValue::Type. Each such type corresponds to exactly one C++ type.
 *
 * Non-primitive value types perform additional memory allocations. To tune the
 * allocation scheme, each TypedValue stores a MemoryResource for said
 * allocations. When copying and moving TypedValue instances, take care that the
 * appropriate MemoryResource is used.
 */
class TypedValue {
 public:
  /** Custom TypedValue equality function that returns a bool
   * (as opposed to returning TypedValue as the default equality does).
   * This implementation treats two nulls as being equal and null
   * not being equal to everything else.
   */
  struct BoolEqual {
    bool operator()(const TypedValue &left, const TypedValue &right) const;
  };

  /** Hash operator for TypedValue.
   *
   * Not injecting into std
   * due to linking problems. If the implementation is in this header,
   * then it implicitly instantiates TypedValue::Value<T> before
   * explicit instantiation in .cpp file. If the implementation is in
   * the .cpp file, it won't link.
   */
  struct Hash {
    size_t operator()(const TypedValue &value) const;
  };

  /** A value type. Each type corresponds to exactly one C++ type */
  enum class Type : unsigned {
    Null,
    Bool,
    Int,
    Double,
    String,
    List,
    Map,
    Vertex,
    Edge,
    Path
  };

  /** Concrete value type of character string */
  using TString =
      std::basic_string<char, std::char_traits<char>, utils::Allocator<char>>;

  /**
   * Unordered set of TypedValue items. Can contain at most one Null element,
   * and treats an integral and floating point value as same if they are equal
   * in the floating point domain (TypedValue operator== behaves the same).
   */
  using unordered_set = std::unordered_set<TypedValue, Hash, BoolEqual>;
  using value_map_t = std::map<std::string, TypedValue>;
  using TVector = std::vector<TypedValue, utils::Allocator<TypedValue>>;

  /** Allocator type so that STL containers are aware that we need one */
  using allocator_type = utils::Allocator<TypedValue>;

  /** Construct a Null value with default utils::NewDeleteResource(). */
  TypedValue() : type_(Type::Null) {}

  /** Construct a Null value with given utils::MemoryResource. */
  explicit TypedValue(utils::MemoryResource *memory)
      : memory_(memory), type_(Type::Null) {}

  // single static reference to Null, used whenever Null should be returned
  // TODO: Remove this as it may be needed to construct TypedValue with a
  // different MemoryResource.
  static const TypedValue Null;

  /**
   * Construct a copy of other.
   * utils::MemoryResource is obtained by calling
   * std::allocator_traits<>::select_on_container_copy_construction(other.memory_).
   * Since we use utils::Allocator, which does not propagate, this means that
   * memory_ will be the default utils::NewDeleteResource().
   */
  TypedValue(const TypedValue &other);

  /** Construct a copy using the given utils::MemoryResource */
  TypedValue(const TypedValue &other, utils::MemoryResource *memory);

  /**
   * Construct with the value of other.
   * utils::MemoryResource is obtained from other. After the move, other will be
   * set to Null.
   */
  TypedValue(TypedValue &&other) noexcept;

  /**
   * Construct with the value of other, but use the given utils::MemoryResource.
   * After the move, other will be set to Null.
   * If `*memory != *other.GetMemoryResource()`, then a copy is made instead of
   * a move.
   */
  TypedValue(TypedValue &&other, utils::MemoryResource *memory);

  // constructors for primitive types
  TypedValue(bool value,
             utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::Bool) {
    bool_v = value;
  }

  TypedValue(int value,
             utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::Int) {
    int_v = value;
  }

  TypedValue(int64_t value,
             utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::Int) {
    int_v = value;
  }

  TypedValue(double value,
             utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::Double) {
    double_v = value;
  }

  // conversion function to PropertyValue
  explicit operator PropertyValue() const;

  // copy constructors for non-primitive types
  TypedValue(const std::string &value,
             utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::String) {
    new (&string_v) TString(value, memory_);
  }

  TypedValue(const char *value,
             utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::String) {
    new (&string_v) TString(value, memory_);
  }

  explicit TypedValue(
      const std::string_view &value,
      utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::String) {
    new (&string_v) TString(value, memory_);
  }

  /**
   * Construct a copy of other.
   * utils::MemoryResource is obtained by calling
   * std::allocator_traits<>::
   *     select_on_container_copy_construction(other.get_allocator()).
   * Since we use utils::Allocator, which does not propagate, this means that
   * memory_ will be the default utils::NewDeleteResource().
   */
  TypedValue(const TString &other)
      : TypedValue(other, std::allocator_traits<utils::Allocator<TypedValue>>::
                              select_on_container_copy_construction(
                                  other.get_allocator())
                                  .GetMemoryResource()) {}

  /** Construct a copy using the given utils::MemoryResource */
  TypedValue(const TString &other, utils::MemoryResource *memory)
      : memory_(memory), type_(Type::String) {
    new (&string_v) TString(other, memory_);
  }

  /** Construct a copy using the given utils::MemoryResource */
  TypedValue(const std::vector<TypedValue> &value,
             utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::List) {
    new (&list_v) TVector(memory_);
    list_v.reserve(value.size());
    list_v.assign(value.begin(), value.end());
  }

  /**
   * Construct a copy of other.
   * utils::MemoryResource is obtained by calling
   * std::allocator_traits<>::
   *     select_on_container_copy_construction(other.get_allocator()).
   * Since we use utils::Allocator, which does not propagate, this means that
   * memory_ will be the default utils::NewDeleteResource().
   */
  TypedValue(const TVector &other)
      : TypedValue(other, std::allocator_traits<utils::Allocator<TypedValue>>::
                              select_on_container_copy_construction(
                                  other.get_allocator())
                                  .GetMemoryResource()) {}

  /** Construct a copy using the given utils::MemoryResource */
  TypedValue(const TVector &value, utils::MemoryResource *memory)
      : memory_(memory), type_(Type::List) {
    new (&list_v) TVector(value, memory_);
  }

  TypedValue(const std::map<std::string, TypedValue> &value,
             utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::Map) {
    new (&map_v) std::map<std::string, TypedValue>(value);
  }

  TypedValue(const VertexAccessor &vertex,
             utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::Vertex) {
    new (&vertex_v) VertexAccessor(vertex);
  }

  TypedValue(const EdgeAccessor &edge,
             utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::Edge) {
    new (&edge_v) EdgeAccessor(edge);
  }

  TypedValue(const Path &path,
             utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::Path) {
    new (&path_v) Path(path);
  }

  /** Construct a copy using default utils::NewDeleteResource() */
  TypedValue(const PropertyValue &value);

  /** Construct a copy using the given utils::MemoryResource */
  TypedValue(const PropertyValue &value, utils::MemoryResource *memory);

  // move constructors for non-primitive types

  /**
   * Construct with the value of other.
   * utils::MemoryResource is obtained from other. After the move, other will be
   * left in unspecified state.
   */
  TypedValue(TString &&other) noexcept
      : TypedValue(std::move(other),
                   other.get_allocator().GetMemoryResource()) {}

  /**
   * Construct with the value of other and use the given MemoryResource
   * After the move, other will be left in unspecified state.
   */
  TypedValue(TString &&other, utils::MemoryResource *memory)
      : memory_(memory), type_(Type::String) {
    new (&string_v) TString(std::move(other), memory_);
  }

  /**
   * Perform an element-wise move using default utils::NewDeleteResource().
   * Other will be not be empty, though elements may be Null.
   */
  TypedValue(std::vector<TypedValue> &&other)
      : TypedValue(std::move(other), utils::NewDeleteResource()) {}

  /**
   * Perform an element-wise move of the other and use the given MemoryResource.
   * Other will be not be empty, though elements may be Null.
   */
  TypedValue(std::vector<TypedValue> &&other, utils::MemoryResource *memory)
      : memory_(memory), type_(Type::List) {
    new (&list_v) TVector(memory_);
    list_v.reserve(other.size());
    // std::vector<TypedValue> has std::allocator and there's no move
    // constructor for std::vector using different allocator types. Since
    // std::allocator is not propagated to elements, it is possible that some
    // TypedValue elements have a MemoryResource that is the same as the one we
    // are given. In such a case we would like to move those TypedValue
    // instances, so we use move_iterator.
    list_v.assign(std::make_move_iterator(other.begin()),
                  std::make_move_iterator(other.end()));
  }

  /**
   * Construct with the value of other.
   * utils::MemoryResource is obtained from other. After the move, other will be
   * left empty.
   */
  TypedValue(TVector &&other) noexcept
      : TypedValue(std::move(other),
                   other.get_allocator().GetMemoryResource()) {}

  /**
   * Construct with the value of other and use the given MemoryResource.
   * If `other.get_allocator() != *memory`, this call will perform an
   * element-wise move and other is not guaranteed to be empty.
   */
  TypedValue(TVector &&other, utils::MemoryResource *memory)
      : memory_(memory), type_(Type::List) {
    new (&list_v) TVector(std::move(other), memory_);
  }

  TypedValue(std::map<std::string, TypedValue> &&value) noexcept
      : type_(Type::Map) {
    new (&map_v) std::map<std::string, TypedValue>(std::move(value));
  }

  TypedValue(std::map<std::string, TypedValue> &&value,
             utils::MemoryResource *memory) noexcept
      : memory_(memory), type_(Type::Map) {
    new (&map_v) std::map<std::string, TypedValue>(std::move(value));
  }

  TypedValue(VertexAccessor &&vertex) noexcept : type_(Type::Vertex) {
    new (&vertex_v) VertexAccessor(std::move(vertex));
  }

  TypedValue(VertexAccessor &&vertex, utils::MemoryResource *memory) noexcept
      : memory_(memory), type_(Type::Vertex) {
    new (&vertex_v) VertexAccessor(std::move(vertex));
  }

  TypedValue(EdgeAccessor &&edge) noexcept : type_(Type::Edge) {
    new (&edge_v) EdgeAccessor(std::move(edge));
  }

  TypedValue(EdgeAccessor &&edge, utils::MemoryResource *memory) noexcept
      : memory_(memory), type_(Type::Edge) {
    new (&edge_v) EdgeAccessor(std::move(edge));
  }

  TypedValue(Path &&path) noexcept : type_(Type::Path) {
    new (&path_v) Path(std::move(path));
  }

  TypedValue(Path &&path, utils::MemoryResource *memory) noexcept
      : memory_(memory), type_(Type::Path) {
    new (&path_v) Path(std::move(path));
  }

  /**
   * Construct with the value of other.
   * Default utils::NewDeleteResource() is used for allocations. After the move,
   * other will be set to Null.
   */
  // NOLINTNEXTLINE(hicpp-explicit-conversions)
  TypedValue(PropertyValue &&other);

  /**
   * Construct with the value of other, but use the given utils::MemoryResource.
   * After the move, other will be set to Null.
   */
  TypedValue(PropertyValue &&other, utils::MemoryResource *memory);

  // copy assignment operators
  TypedValue &operator=(const char *);
  TypedValue &operator=(int);
  TypedValue &operator=(bool);
  TypedValue &operator=(int64_t);
  TypedValue &operator=(double);
  TypedValue &operator=(const TString &);
  TypedValue &operator=(const std::string &);
  TypedValue &operator=(const std::string_view &);
  TypedValue &operator=(const TVector &);
  TypedValue &operator=(const std::vector<TypedValue> &);
  TypedValue &operator=(const TypedValue::value_map_t &);
  TypedValue &operator=(const VertexAccessor &);
  TypedValue &operator=(const EdgeAccessor &);
  TypedValue &operator=(const Path &);

  /** Copy assign other, utils::MemoryResource of `this` is used */
  TypedValue &operator=(const TypedValue &other);

  // move assignment operators
  TypedValue &operator=(TString &&);
  TypedValue &operator=(TVector &&);
  TypedValue &operator=(std::vector<TypedValue> &&);
  TypedValue &operator=(TypedValue::value_map_t &&);
  TypedValue &operator=(VertexAccessor &&);
  TypedValue &operator=(EdgeAccessor &&);
  TypedValue &operator=(Path &&);

  /** Move assign other, utils::MemoryResource of `this` is used. */
  TypedValue &operator=(TypedValue &&other) noexcept(false);

  ~TypedValue();

  Type type() const { return type_; }

  /**
   * Returns the value of the property as given type T.
   * The behavior of this function is undefined if
   * T does not correspond to this property's type_.
   *
   * @tparam T Type to interpret the value as.
   * @return The value as type T.
   */
  template <typename T>
  T &Value();
  template <typename T>
  const T &Value() const;

  // TODO consider adding getters for primitives by value (and not by ref)

#define DECLARE_VALUE_AND_TYPE_GETTERS(type_param, field)          \
  /** Gets the value of type field. Throws if value is not field*/ \
  type_param &Value##field();                                      \
  /** Gets the value of type field. Throws if value is not field*/ \
  const type_param &Value##field() const;                          \
  /** Checks if it's the value is of the given type */             \
  bool Is##field() const;

  DECLARE_VALUE_AND_TYPE_GETTERS(bool, Bool)
  DECLARE_VALUE_AND_TYPE_GETTERS(int64_t, Int)
  DECLARE_VALUE_AND_TYPE_GETTERS(double, Double)
  DECLARE_VALUE_AND_TYPE_GETTERS(TString, String)

  /**
   * Get the list value.
   * @throw TypedValueException if stored value is not a list.
   */
  TVector &ValueList();

  const TVector &ValueList() const;

  /** Check if the stored value is a list value */
  bool IsList() const;

  DECLARE_VALUE_AND_TYPE_GETTERS(value_map_t, Map)
  DECLARE_VALUE_AND_TYPE_GETTERS(VertexAccessor, Vertex)
  DECLARE_VALUE_AND_TYPE_GETTERS(EdgeAccessor, Edge)
  DECLARE_VALUE_AND_TYPE_GETTERS(Path, Path)

#undef DECLARE_VALUE_AND_TYPE_GETTERS

  /**  Checks if value is a TypedValue::Null. */
  bool IsNull() const;

  /** Convenience function for checking if this TypedValue is either
   * an integer or double */
  bool IsNumeric() const;

  /** Convenience function for checking if this TypedValue can be converted into
   * PropertyValue */
  bool IsPropertyValue() const;

  utils::MemoryResource *GetMemoryResource() const { return memory_; }

 private:
  void DestroyValue();

  // Memory resource for allocations of non primitive values
  utils::MemoryResource *memory_{utils::NewDeleteResource()};

  // storage for the value of the property
  union {
    bool bool_v;
    int64_t int_v;
    double double_v;
    // Since this is used in query runtime, size of union is not critical so
    // string and vector are used instead of pointers. It requires copy of data,
    // but most of algorithms (concatenations, serialisation...) has linear time
    // complexity so it shouldn't be a problem. This is maybe even faster
    // because of data locality.
    TString string_v;
    TVector list_v;
    // clang doesn't allow unordered_map to have incomplete type as value so we
    // we use map.
    std::map<std::string, TypedValue> map_v;
    VertexAccessor vertex_v;
    EdgeAccessor edge_v;
    Path path_v;
  };

  /**
   * The Type of property.
   */
  Type type_;
};

/**
 * An exception raised by the TypedValue system. Typically when
 * trying to perform operations (such as addition) on TypedValues
 * of incompatible Types.
 */
class TypedValueException : public utils::BasicException {
 public:
  using utils::BasicException::BasicException;
};

// binary bool operators

/**
 * Perform logical 'and' on TypedValues.
 *
 * If any of the values is false, return false. Otherwise checks if any value is
 * Null and return Null. All other cases return true. The resulting value uses
 * the same MemoryResource as the left hand side arguments.
 *
 * @throw TypedValueException if arguments are not boolean or Null.
 */
TypedValue operator&&(const TypedValue &a, const TypedValue &b);

/**
 * Perform logical 'or' on TypedValues.
 *
 * If any of the values is true, return true. Otherwise checks if any value is
 * Null and return Null. All other cases return false. The resulting value uses
 * the same MemoryResource as the left hand side arguments.
 *
 * @throw TypedValueException if arguments are not boolean or Null.
 */
TypedValue operator||(const TypedValue &a, const TypedValue &b);

/**
 * Logically negate a TypedValue.
 *
 * Negating Null value returns Null. Values other than null raise an exception.
 * The resulting value uses the same MemoryResource as the argument.
 *
 * @throw TypedValueException if TypedValue is not a boolean or Null.
 */
TypedValue operator!(const TypedValue &a);

// binary bool xor, not power operator
// Be careful: since ^ is binary operator and || and && are logical operators
// they have different priority in c++.
TypedValue operator^(const TypedValue &a, const TypedValue &b);

// comparison operators

/**
 * Compare TypedValues and return true, false or Null.
 *
 * Null is returned if either of the two values is Null.
 * Since each TypedValue may have a different MemoryResource for allocations,
 * the results is allocated using MemoryResource obtained from the left hand
 * side.
 */
TypedValue operator==(const TypedValue &a, const TypedValue &b);

/**
 * Compare TypedValues and return true, false or Null.
 *
 * Null is returned if either of the two values is Null.
 * Since each TypedValue may have a different MemoryResource for allocations,
 * the results is allocated using MemoryResource obtained from the left hand
 * side.
 */
inline TypedValue operator!=(const TypedValue &a, const TypedValue &b) {
  return !(a == b);
}

/**
 * Compare TypedValues and return true, false or Null.
 *
 * Null is returned if either of the two values is Null.
 * The resulting value uses the same MemoryResource as the left hand side
 * argument.
 *
 * @throw TypedValueException if the values cannot be compared, i.e. they are
 *        not either Null, numeric or a character string type.
 */
TypedValue operator<(const TypedValue &a, const TypedValue &b);

/**
 * Compare TypedValues and return true, false or Null.
 *
 * Null is returned if either of the two values is Null.
 * The resulting value uses the same MemoryResource as the left hand side
 * argument.
 *
 * @throw TypedValueException if the values cannot be compared, i.e. they are
 *        not either Null, numeric or a character string type.
 */
inline TypedValue operator<=(const TypedValue &a, const TypedValue &b) {
  return a < b || a == b;
}

/**
 * Compare TypedValues and return true, false or Null.
 *
 * Null is returned if either of the two values is Null.
 * The resulting value uses the same MemoryResource as the left hand side
 * argument.
 *
 * @throw TypedValueException if the values cannot be compared, i.e. they are
 *        not either Null, numeric or a character string type.
 */
inline TypedValue operator>(const TypedValue &a, const TypedValue &b) {
  return !(a <= b);
}

/**
 * Compare TypedValues and return true, false or Null.
 *
 * Null is returned if either of the two values is Null.
 * The resulting value uses the same MemoryResource as the left hand side
 * argument.
 *
 * @throw TypedValueException if the values cannot be compared, i.e. they are
 *        not either Null, numeric or a character string type.
 */
inline TypedValue operator>=(const TypedValue &a, const TypedValue &b) {
  return !(a < b);
}

// arithmetic operators

/**
 * Arithmetically negate a value.
 *
 * If the value is Null, then Null is returned.
 * The resulting value uses the same MemoryResource as the argument.
 *
 * @throw TypedValueException if the value is not numeric or Null.
 */
TypedValue operator-(const TypedValue &a);

/**
 * Apply the unary plus operator to a value.
 *
 * If the value is Null, then Null is returned.
 * The resulting value uses the same MemoryResource as the argument.
 *
 * @throw TypedValueException if the value is not numeric or Null.
 */
TypedValue operator+(const TypedValue &a);

/**
 * Perform addition or concatenation on two values.
 *
 * Numeric values are summed, while lists and character strings are
 * concatenated. If either value is Null, then Null is returned. The resulting
 * value uses the same MemoryResource as the left hand side argument.
 *
 * @throw TypedValueException if values cannot be summed or concatenated.
 */
TypedValue operator+(const TypedValue &a, const TypedValue &b);

/**
 * Subtract two values.
 *
 * If any of the values is Null, then Null is returned.
 * The resulting value uses the same MemoryResource as the left hand side
 * argument.
 *
 * @throw TypedValueException if the values are not numeric or Null.
 */
TypedValue operator-(const TypedValue &a, const TypedValue &b);

/**
 * Divide two values.
 *
 * If any of the values is Null, then Null is returned.
 * The resulting value uses the same MemoryResource as the left hand side
 * argument.
 *
 * @throw TypedValueException if the values are not numeric or Null, or if
 *        dividing two integer values by zero.
 */
TypedValue operator/(const TypedValue &a, const TypedValue &b);

/**
 * Multiply two values.
 *
 * If any of the values is Null, then Null is returned.
 * The resulting value uses the same MemoryResource as the left hand side
 * argument.
 *
 * @throw TypedValueException if the values are not numeric or Null.
 */
TypedValue operator*(const TypedValue &a, const TypedValue &b);

/**
 * Perform modulo operation on two values.
 *
 * If any of the values is Null, then Null is returned.
 * The resulting value uses the same MemoryResource as the left hand side
 * argument.
 *
 * @throw TypedValueException if the values are not numeric or Null.
 */
TypedValue operator%(const TypedValue &a, const TypedValue &b);

/** Output the TypedValue::Type value as a string */
std::ostream &operator<<(std::ostream &os, const TypedValue::Type &type);

/**
 * Output the TypedValue value as a readable string.
 * Note that the primary use of this is for debugging and may not yield the
 * pretty results you want to display to the user.
 */
std::ostream &operator<<(std::ostream &os, const TypedValue &value);

}  // namespace query
