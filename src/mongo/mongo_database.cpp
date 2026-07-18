#include "minidb/mongo/mongo_database.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace minidb::mongo {
namespace {

template <class... Visitors>
struct Overloaded : Visitors... {
    using Visitors::operator()...;
};
template <class... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

enum class TransactionStatus { in_progress, committed, rolled_back };

struct Snapshot {
    std::uint64_t xmax{};
    std::unordered_set<std::uint64_t> active_transactions;
};

struct TransactionRecord {
    TransactionStatus status{TransactionStatus::in_progress};
    Snapshot snapshot;
};

struct DocumentVersion {
    Document document;
    std::uint64_t created_by{};
};

// created_by 演示事务内隐式创建 collection：提交后归零，回滚时删除。
struct Collection {
    std::uint64_t created_by{};
    std::vector<DocumentVersion> versions;
};

bool valid_collection_name(const std::string& name) {
    if (name.empty() ||
        (!std::isalpha(static_cast<unsigned char>(name.front())) && name.front() != '_')) {
        return false;
    }
    return std::all_of(name.begin() + 1, name.end(), [](unsigned char character) {
        return std::isalnum(character) || character == '_';
    });
}

bool matches_filter(const Document& document, const Document& filter) {
    for (const auto& [key, expected] : filter) {
        const auto iterator = document.find(key);
        if (iterator == document.end() || iterator->second != expected) {
            return false;
        }
    }
    return true;
}

}  // namespace

struct MongoDatabase::Impl {
    std::mutex mutex;
    std::uint64_t next_transaction_id{1};
    std::uint64_t next_session_id{30001};
    std::uint64_t next_document_id{1};
    std::unordered_map<std::uint64_t, TransactionRecord> transactions;
    std::unordered_map<std::string, Collection> collections;

    Snapshot capture_snapshot(std::optional<std::uint64_t> self) const {
        Snapshot snapshot;
        snapshot.xmax = next_transaction_id;
        for (const auto& [id, transaction] : transactions) {
            if (transaction.status == TransactionStatus::in_progress &&
                (!self || id != *self)) {
                snapshot.active_transactions.insert(id);
            }
        }
        return snapshot;
    }

    std::uint64_t begin_transaction() {
        const auto id = next_transaction_id++;
        transactions.emplace(
            id, TransactionRecord{TransactionStatus::in_progress, capture_snapshot(id)});
        return id;
    }

    void commit_transaction(std::uint64_t id) {
        transactions.at(id).status = TransactionStatus::committed;
        for (auto& [name, collection] : collections) {
            (void)name;
            if (collection.created_by == id) {
                collection.created_by = 0;
            }
        }
    }

    void rollback_transaction(std::uint64_t id) {
        transactions.at(id).status = TransactionStatus::rolled_back;
        std::erase_if(collections,
                      [id](const auto& item) { return item.second.created_by == id; });
    }

    bool collection_visible(const Collection& collection,
                            std::optional<std::uint64_t> self) const {
        return collection.created_by == 0 || (self && collection.created_by == *self);
    }

    bool document_visible(const DocumentVersion& version,
                          const Snapshot& snapshot,
                          std::optional<std::uint64_t> self) const {
        if (self && version.created_by == *self) {
            return true;
        }
        const auto transaction = transactions.find(version.created_by);
        return transaction != transactions.end() &&
               transaction->second.status == TransactionStatus::committed &&
               version.created_by < snapshot.xmax &&
               !snapshot.active_transactions.contains(version.created_by);
    }

    bool duplicate_id(const Collection& collection,
                      const BsonValue& id,
                      std::uint64_t self) const {
        for (const auto& version : collection.versions) {
            const auto field = version.document.find("_id");
            if (field == version.document.end() || field->second != id) {
                continue;
            }
            const auto transaction = transactions.find(version.created_by);
            if (version.created_by == self ||
                (transaction != transactions.end() &&
                 transaction->second.status != TransactionStatus::rolled_back)) {
                return true;
            }
        }
        return false;
    }
};

MongoDatabase::MongoDatabase() : impl_(std::make_unique<Impl>()) {}
MongoDatabase::~MongoDatabase() = default;

std::unique_ptr<MongoSession> MongoDatabase::connect() {
    std::lock_guard lock(impl_->mutex);
    return std::unique_ptr<MongoSession>(new MongoSession(*this, impl_->next_session_id++));
}

MongoSession::MongoSession(MongoDatabase& database, std::uint64_t session_id)
    : database_(&database), session_id_(session_id) {}

MongoSession::~MongoSession() {
    if (database_ != nullptr) {
        database_->rollback_on_disconnect(*this);
    }
}

QueryResult MongoSession::execute(const std::string& command) {
    const auto parsed = parse_command(command);
    if (!parsed.ok()) {
        return QueryResult::error(parsed.error);
    }
    return database_->execute(*this, *parsed.statement);
}

void MongoDatabase::rollback_on_disconnect(MongoSession& session) {
    std::lock_guard lock(impl_->mutex);
    if (session.transaction_id_) {
        impl_->rollback_transaction(*session.transaction_id_);
        session.transaction_id_.reset();
    }
}

QueryResult MongoDatabase::execute(MongoSession& session, const Statement& statement) {
    std::lock_guard lock(impl_->mutex);

    return std::visit(Overloaded{
        [&](const CreateCollectionStatement& create) -> QueryResult {
            if (!valid_collection_name(create.collection)) {
                return QueryResult::error(
                    "collection 名称只能使用字母、数字和下划线，且不能以数字开头");
            }
            if (impl_->collections.contains(create.collection)) {
                return QueryResult::error("collection 已存在: " + create.collection);
            }

            const bool autocommit = !session.transaction_id_;
            if (autocommit) {
                session.transaction_id_ = impl_->begin_transaction();
            }
            const auto transaction_id = *session.transaction_id_;
            impl_->collections.emplace(create.collection,
                                       Collection{transaction_id, {}});
            if (autocommit) {
                impl_->commit_transaction(transaction_id);
                session.transaction_id_.reset();
            }
            return QueryResult::success(
                "{ ok: 1, collection: \"" + create.collection + "\" }");
        },
        [&](const InsertOneStatement& insert) -> QueryResult {
            if (!valid_collection_name(insert.collection)) {
                return QueryResult::error("无效的 collection 名称: " + insert.collection);
            }

            const bool autocommit = !session.transaction_id_;
            if (autocommit) {
                session.transaction_id_ = impl_->begin_transaction();
            }
            const auto transaction_id = *session.transaction_id_;

            auto collection = impl_->collections.find(insert.collection);
            if (collection == impl_->collections.end()) {
                collection = impl_->collections
                                 .emplace(insert.collection,
                                          Collection{transaction_id, {}})
                                 .first;
            } else if (!impl_->collection_visible(collection->second,
                                                  session.transaction_id_)) {
                if (autocommit) {
                    impl_->rollback_transaction(transaction_id);
                    session.transaction_id_.reset();
                }
                return QueryResult::error("collection 正由另一个事务创建: " +
                                          insert.collection);
            }

            auto document = insert.document;
            auto id = document.find("_id");
            if (id == document.end()) {
                BsonValue generated_id;
                do {
                    generated_id = BsonValue(static_cast<std::int64_t>(
                        impl_->next_document_id++));
                } while (impl_->duplicate_id(collection->second, generated_id,
                                             transaction_id));
                id = document.emplace("_id", std::move(generated_id)).first;
            } else if (!std::holds_alternative<std::int64_t>(id->second.value) &&
                       !std::holds_alternative<std::string>(id->second.value)) {
                if (autocommit) {
                    impl_->rollback_transaction(transaction_id);
                    session.transaction_id_.reset();
                }
                return QueryResult::error("教学实现的 _id 仅支持整数或字符串");
            }

            if (impl_->duplicate_id(collection->second, id->second, transaction_id)) {
                if (autocommit) {
                    impl_->rollback_transaction(transaction_id);
                    session.transaction_id_.reset();
                }
                return QueryResult::error("E11000 duplicate key _id: " +
                                          bson_to_json(id->second));
            }

            const auto rendered_id = bson_to_json(id->second);
            collection->second.versions.push_back(
                DocumentVersion{std::move(document), transaction_id});
            if (autocommit) {
                impl_->commit_transaction(transaction_id);
                session.transaction_id_.reset();
            }
            return QueryResult::success(
                "{ acknowledged: true, insertedId: " + rendered_id + " }");
        },
        [&](const FindStatement& find) -> QueryResult {
            const auto collection = impl_->collections.find(find.collection);
            if (collection == impl_->collections.end() ||
                !impl_->collection_visible(collection->second,
                                           session.transaction_id_)) {
                return QueryResult::error("collection 不存在: " + find.collection);
            }

            const Snapshot snapshot = session.transaction_id_
                                          ? impl_->transactions
                                                .at(*session.transaction_id_)
                                                .snapshot
                                          : impl_->capture_snapshot(std::nullopt);
            std::vector<Row> rows;
            for (const auto& version : collection->second.versions) {
                if (impl_->document_visible(version, snapshot,
                                            session.transaction_id_) &&
                    matches_filter(version.document, find.filter)) {
                    rows.push_back(Row{document_to_json(version.document)});
                }
            }
            return QueryResult::table(
                {"document"}, std::move(rows),
                "find returned documents（灵活 schema；顶层字段精确匹配）");
        },
        [&](const BeginStatement&) -> QueryResult {
            if (session.transaction_id_) {
                return QueryResult::error("session 已经处于事务中");
            }
            session.transaction_id_ = impl_->begin_transaction();
            return QueryResult::success(
                "transaction started; txn_id=" +
                std::to_string(*session.transaction_id_) +
                "（教学版 snapshot transaction）");
        },
        [&](const CommitStatement&) -> QueryResult {
            if (!session.transaction_id_) {
                return QueryResult::success("commitTransaction（当前没有事务）");
            }
            const auto id = *session.transaction_id_;
            impl_->commit_transaction(id);
            session.transaction_id_.reset();
            return QueryResult::success("transaction committed; txn_id=" +
                                        std::to_string(id));
        },
        [&](const RollbackStatement&) -> QueryResult {
            if (!session.transaction_id_) {
                return QueryResult::success("abortTransaction（当前没有事务）");
            }
            const auto id = *session.transaction_id_;
            impl_->rollback_transaction(id);
            session.transaction_id_.reset();
            return QueryResult::success("transaction aborted; txn_id=" +
                                        std::to_string(id));
        },
        [&](const ShowCollectionsStatement&) -> QueryResult {
            std::vector<std::string> names;
            for (const auto& [name, collection] : impl_->collections) {
                if (impl_->collection_visible(collection, session.transaction_id_)) {
                    names.push_back(name);
                }
            }
            std::sort(names.begin(), names.end());
            std::vector<Row> rows;
            rows.reserve(names.size());
            for (auto& name : names) {
                rows.push_back(Row{std::move(name)});
            }
            return QueryResult::table({"collection"}, std::move(rows),
                                      "show collections");
        },
        [&](const ShowArchitectureStatement&) -> QueryResult {
            return QueryResult::table(
                {"组件", "本项目中的体现", "真实 MongoDB 对应物"},
                {
                    Row{std::string("mongod"), std::string("MongoDatabase"),
                        std::string("数据库服务进程")},
                    Row{std::string("driver session"),
                        std::string("MongoSession id=") +
                            std::to_string(session.session_id_),
                        std::string("客户端逻辑会话")},
                    Row{std::string("collection"),
                        std::string("unordered_map<string, Collection>"),
                        std::string("无固定列的文档集合")},
                    Row{std::string("BSON document"),
                        std::string("map + recursive variant"),
                        std::string("嵌套对象与数组")},
                    Row{std::string("transaction"),
                        std::string("document version + snapshot"),
                        std::string("单文档原子性/多文档事务")},
                },
                "文档模型：访问模式驱动的嵌入数据，而不是固定行列和 JOIN");
        },
        [&](const HelpStatement&) -> QueryResult {
            return QueryResult::success(
                "支持语法:\n"
                "  db.createCollection('users');\n"
                "  db.users.insertOne({name: 'Alice', tags: ['vip']});\n"
                "  db.users.find();  db.users.find({name: 'Alice'});\n"
                "  BEGIN; COMMIT; ROLLBACK;\n"
                "  session.startTransaction();\n"
                "  session.commitTransaction(); session.abortTransaction();\n"
                "  SHOW COLLECTIONS; SHOW ARCHITECTURE; HELP; QUIT;");
        },
        [&](const QuitStatement&) -> QueryResult {
            return QueryResult::success("QUIT");
        },
    }, statement);
}

}  // namespace minidb::mongo
