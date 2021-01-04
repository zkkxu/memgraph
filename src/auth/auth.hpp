#pragma once

#include <mutex>
#include <optional>
#include <vector>

#include "auth/exceptions.hpp"
#include "auth/models.hpp"
#include "auth/module.hpp"
#include "communication/context.hpp"
#include "kvstore/kvstore.hpp"

#include "rpc/client.hpp"
#include "rpc/server.hpp"

namespace auth {

/**
 * This class serves as the main Authentication/Authorization storage.
 * It provides functions for managing Users, Roles and Permissions.
 * NOTE: The functions in this class aren't thread safe. Use the `WithLock` lock
 * if you want to have safe modifications of the storage.
 * TODO (mferencevic): Disable user/role modification functions when they are
 * being managed by the auth module.
 */
class Auth final {
 public:
  Auth(const std::string &storage_directory);

  /**
   * Authenticates a user using his username and password.
   *
   * @param username
   * @param password
   *
   * @return a user when the username and password match, nullopt otherwise
   * @throw AuthException if unable to authenticate for whatever reason.
   */
  std::optional<User> Authenticate(const std::string &username,
                                   const std::string &password);

  /**
   * Gets a user from the storage.
   *
   * @param username
   *
   * @return a user when the user exists, nullopt otherwise
   * @throw AuthException if unable to load user data.
   */
  std::optional<User> GetUser(const std::string &username);

  /**
   * Saves a user object to the storage.
   *
   * @param user
   *
   * @throw AuthException if unable to save the user.
   */
  void SaveUser(const User &user);

  /**
   * Creates a user if the user doesn't exist.
   *
   * @param username
   * @param password
   *
   * @return a user when the user is created, nullopt if the user exists
   * @throw AuthException if unable to save the user.
   */
  std::optional<User> AddUser(
      const std::string &username,
      const std::optional<std::string> &password = std::nullopt);

  /**
   * Removes a user from the storage.
   *
   * @param username
   *
   * @return `true` if the user existed and was removed, `false` if the user
   *         doesn't exist
   * @throw AuthException if unable to remove the user.
   */
  bool RemoveUser(const std::string &username);

  /**
   * Gets all users from the storage.
   *
   * @return a list of users
   * @throw AuthException if unable to load user data.
   */
  std::vector<User> AllUsers();

  /**
   * Returns whether there are users in the storage.
   *
   * @return `true` if the storage contains any users, `false` otherwise
   */
  bool HasUsers();

  /**
   * Gets a role from the storage.
   *
   * @param rolename
   *
   * @return a role when the role exists, nullopt otherwise
   * @throw AuthException if unable to load role data.
   */
  std::optional<Role> GetRole(const std::string &rolename);

  /**
   * Saves a role object to the storage.
   *
   * @param role
   *
   * @throw AuthException if unable to save the role.
   */
  void SaveRole(const Role &role);

  /**
   * Creates a role if the role doesn't exist.
   *
   * @param rolename
   *
   * @return a role when the role is created, nullopt if the role exists
   * @throw AuthException if unable to save the role.
   */
  std::optional<Role> AddRole(const std::string &rolename);

  /**
   * Removes a role from the storage.
   *
   * @param rolename
   *
   * @return `true` if the role existed and was removed, `false` if the role
   *         doesn't exist
   * @throw AuthException if unable to remove the role.
   */
  bool RemoveRole(const std::string &rolename);

  /**
   * Gets all roles from the storage.
   *
   * @return a list of roles
   * @throw AuthException if unable to load role data.
   */
  std::vector<Role> AllRoles();

  /**
   * Gets all users for a role from the storage.
   *
   * @param rolename
   *
   * @return a list of roles
   * @throw AuthException if unable to load user data.
   */
  std::vector<User> AllUsersForRole(const std::string &rolename);

  /**
   * Returns a reference to the lock that should be used for all operations that
   * require more than one interaction with this class.
   */
  std::mutex &WithLock();

 private:
  kvstore::KVStore storage_;
  auth::Module module_;
  // Even though the `kvstore::KVStore` class is guaranteed to be thread-safe we
  // use a mutex to lock all operations on the `User` and `Role` storage because
  // some operations on the users and/or roles may require more than one
  // operation on the storage.
  std::mutex lock_;
};
}  // namespace auth
