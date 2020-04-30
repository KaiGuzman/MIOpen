/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (c) 2019 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/
#include <miopen/sqlite_db.hpp>
#include <miopen/db_record.hpp>
#include <miopen/errors.hpp>
#include <miopen/lock_file.hpp>
#include <miopen/logger.hpp>
#include <miopen/md5.hpp>
#include <miopen/problem_description.hpp>

#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>

#include <memory>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <ios>
#include <mutex>
#include <shared_mutex>
#include <string>

namespace miopen {

class SQLite::impl
{
    struct SQLiteCloser
    {
        void operator()(sqlite3* ptr)
        {
            std::string filename_(sqlite3_db_filename(ptr, "main"));
            SQLite::Retry([&]() { return sqlite3_close(ptr); }, filename_);
        }
    };

    public:
    impl(const std::string& filename_)
    {
        sqlite3* ptr_tmp;
        int rc = 0;
        if(is_system)
            rc = sqlite3_open_v2(filename_.c_str(), &ptr_tmp, SQLITE_OPEN_READONLY, nullptr);
        else
            rc = sqlite3_open_v2(
                filename_.c_str(), &ptr_tmp, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        ptrDb   = sqlite3_ptr{ptr_tmp};
        isValid = (rc == 0);
    }
    bool Exec(const std::string& query) const
    {
        MIOPEN_LOG_T(std::this_thread::get_id() << ":" << query);
        auto rc = Retry([&]() {
            return sqlite3_exec(ptrDb.get(), query.c_str(), find_callback, nullptr, nullptr);
        });
        if(rc != SQLITE_OK)
        {
            MIOPEN_LOG_I2(query);
            MIOPEN_THROW(miopenStatusInternalError, SQLErrorMessage());
            sqlite3_close(ptrDb.get());
            return false;
        }
        return true;
    }
    bool Exec(const std::string& query, SQLRes_t& res) const
    {
        res.clear();
        MIOPEN_LOG_T(std::this_thread::get_id() << ":" << query);
        {
            auto rc = Retry([&]() {
                return sqlite3_exec(
                    ptrDb.get(), query.c_str(), find_callback, static_cast<void*>(&res), nullptr);
            });
            if(rc != SQLITE_OK)
            {
                MIOPEN_LOG_I2(query);
                MIOPEN_THROW(miopenStatusInternalError, SQLErrorMessage());
                sqlite3_close(ptrDb.get());
                return false;
            }
        }
        return true;
    }

    static int find_callback(void* _res, int argc, char** argv, char** azColName)
    {
        SQLRes_t* res = static_cast<SQLRes_t*>(_res);
        std::unordered_map<std::string, std::string> record;
        for(auto i               = 0; i < argc; i++)
            record[azColName[i]] = (argv[i] != nullptr) ? argv[i] : "NULL";
        res->push_back(record);
        return 0;
    }
    int Retry(std::function<int()> f) const { return SQLiteBase::SQLRety(f, filename); }

    static int Retry(std::function<int()> f, std::string filename) const
    {
        auto timeout_end = std::chrono::high_resolution_clock::now() +
                           std::chrono::seconds(30); // TODO: make configurable
        auto tries = 0;
        while(true)
        {
            int rc = f();
            if(rc == SQLITE_BUSY)
            {
                MIOPEN_LOG_I2("Database" + filename + "  busy, retrying ...");
                ++tries;
                if(tries > 50)
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                else
                    std::this_thread::yield();
            }
            else
                return rc;
            if(std::chrono::high_resolution_clock::now() > timeout_end)
                MIOPEN_THROW("Timeout while waiting for Database: " + filename);
        }
    }

    int Changes() const { return sqlite3_changes(ptrDb.get()); }

    std::string ErrorMessage() const
    {
        std::string errMsg = "Internal error while accessing SQLite database: ";
        return errMsg + sqlite3_errmsg(ptrDb.get());
    }

    protected:
    using sqlite3_ptr = std::unique_ptr<sqlite3, SQLiteCloser>;
    sqlite3_ptr ptrDb = nullptr;
    bool isValid;
};

class SQLiteStmt::impl
{
    sqlite3_stmt_ptr Prepare(const SQL& sql, const std::string& query)
    {
        sqlite3_stmt* ptr = nullptr;
        MIOPEN_LOG_I2(query);
        auto rc =
            sqlite3_prepare_v2(sql.pImpl->ptrDb.get(), query.c_str(), query.size(), &ptr, nullptr);
        if(rc != SQLITE_OK)
        {
            std::string err_msg = "SQLite prepare error: ";
            MIOPEN_THROW(miopenStatusInternalError, err_msg + sql.ErrorMessage());
        }
        return sqlite3_stmt_ptr{ptr};
    }

    public:
    using sqlite3_stmt_ptr = MIOPEN_MANAGE_PTR(sqlite3_stmt*, sqlite3_finalize);
    SQLiteStmt(const SQL& sql, const std::string& query) { ptrStmt = Prepare(sql, query); }
    SQLiteStmt(const SQL& sql, const std::string& query, const std::vector<std::string>& vals)
    {
        ptrStmt = Prepare(sql, query);
        int cnt = 1;
        for(auto& kinder : values)
        {
            auto rc = sqlite3_bind_text(
                stmt.get(), cnt++, kinder.data(), kinder.size(), SQLITE_TRANSIENT); // NOLINT
            if(rc != SQLITE_OK)
                MIOPEN_THROW(miopenStatusInternalError, SQLErrorMessage());
        }
        MIOPEN_LOG_I2("[" << JoinStrings(values, ",") << "]");
    }

    int Step()
    {
        return SQL::Retry([&]() { return sqlite3_step(ptrStmt.get()); });
    }
    std::string ColumnText(int idx)
    {
        return std::string{reinterpret_cast<const char*>(sqlite3_column_text(ptrStmt.get(), idx)),
                           sqlite3_column_bytes(ptrStmt.get(), idx)};
    }

    std::string ColumnBlob(int idx)
    {
        auto ptr = sqlite3_column_blob(pStmt.get(), 0);
        auto sz  = sqlite3_column_bytes(pStmt.get(), 0);
        return std::string{reinterpret_cast<const char*>(ptr), sz};
    }
    int64_t ColumnInt64(int idx) { return sqlite3_column_int64(ptrStmt.get(), idx); }

    int BindText(int idx, const std::string& txt)
    {
        sqlite3_bind_text(ptrStmt.get(), idx, txt.data(), txt.size(), SQLITE_TRANSIENT); // NOLINT
        return 0;
    }
    int BindBlob(int idx, const std::string& blob)
    {
        sqlite3_bind_blob(ptrStmt.get(), idx, blob.data(), blob.size(), SQLITE_TRANSIENT); // NOLINT
        return 0;
    }

    protected:
    sqlite3_stmt_ptr ptrStmt = nullptr;
};

SQLitePerfDb::SQLitePerfDb(const std::string& filename_,
                           bool is_system,
                           const std::string& arch_,
                           const std::size_t num_cu_)
    : SQLiteBase(filename_, is_system, arch_, num_cu_)
{
    ProblemDescription prob_desc{conv::Direction::Forward};
    prob_desc.in_data_type      = miopenFloat;
    prob_desc.out_data_type     = miopenFloat;
    prob_desc.weights_data_type = miopenFloat;
    if(!is_system)
    {
        SQLRes_t res;
        const std::string create_config = prob_desc.CreateQuery();
        // clang-format off
        const std::string create_perfdb_sql =
            "CREATE TABLE  IF NOT EXISTS `perf_db` ("
            "`id` INTEGER PRIMARY KEY ASC,"
            "`solver` TEXT NOT NULL,"
            "`config` INTEGER NOT NULL,"
            "`arch` TEXT NOT NULL,"
            "`num_cu` INTEGER NOT NULL,"
            "`params` TEXT NOT NULL"
            ");"
            "CREATE UNIQUE INDEX IF NOT EXISTS "
            "`idx_perf_db` "
            "ON perf_db(solver, config, arch, num_cu);";

        // clang-format on
        {
            const auto lock = shared_lock(lock_file, GetLockTimeout());
            MIOPEN_VALIDATE_LOCK(lock);
            // clang-format off
            const auto check_tables =
                "SELECT name FROM sqlite_master "
                "WHERE "
                  "type = 'table' AND "
                  "(name = 'config' OR name = 'perf_db');";
            // clang-format on
            SQLExec(check_tables, res);
        }
        if(res.empty())
        {
            const auto lock = exclusive_lock(lock_file, GetLockTimeout());
            MIOPEN_VALIDATE_LOCK(lock);
            if(!SQLExec(create_config + create_perfdb_sql))
                MIOPEN_THROW(miopenStatusInternalError);
            MIOPEN_LOG_I2("Database created successfully");
        }
    }
    // Check fields for the tables
    if(!dbInvalid)
    {
        if(!CheckTableColumns(ProblemDescription::table_name(), prob_desc.FieldNames()))
        {
            std::ostringstream ss;
            ss << "Invalid fields in table: " << ProblemDescription::table_name()
               << " disabling access to " << filename;
            MIOPEN_LOG_W(ss.str());
            dbInvalid = true;
        }
        if(!CheckTableColumns("perf_db", {"solver", "config", "arch", "num_cu", "params"}))
        {
            MIOPEN_LOG_W("Invalid fields in table: perf_db disabling access to " + filename);
            dbInvalid = true;
        }
    }
    else
        MIOPEN_LOG_I(filename + " database invalid");
}
} // namespace miopen
