#include <qpdf/QPDFJob.hh>
#include <qpdf/JSONHandler.hh>
#include <qpdf/QPDFUsage.hh>
#include <qpdf/QUtil.hh>
#include <qpdf/QTC.hh>

#include <memory>
#include <stdexcept>
#include <sstream>
#include <cstring>

static JSON JOB_SCHEMA = JSON::parse(QPDFJob::job_json_schema_v1().c_str());

namespace
{
    class Handlers
    {
      public:
        Handlers(bool partial, std::shared_ptr<QPDFJob::Config> c_main);
        void handle(JSON&);

      private:
#       include <qpdf/auto_job_json_decl.hh>

        void usage(std::string const& message);
        void initHandlers();

        typedef std::function<void()> bare_handler_t;
        typedef std::function<void(char const*)> param_handler_t;
        typedef std::function<void(JSON)> json_handler_t;

        void addBare(bare_handler_t);
        void addParameter(param_handler_t);
        void addChoices(char const** choices, bool required, param_handler_t);
        void pushKey(std::string const& key);
        void beginDict(json_handler_t start_fn,
                       bare_handler_t end_fn);
        void beginArray(json_handler_t start_fn,
                        bare_handler_t end_fn);
        void ignoreItem();
        void popHandler();

        bare_handler_t bindBare(void (Handlers::*f)());
        json_handler_t bindJSON(void (Handlers::*f)(JSON));

        std::list<std::shared_ptr<JSONHandler>> json_handlers;
        bool partial;
        JSONHandler* jh; // points to last of json_handlers
        std::shared_ptr<QPDFJob::Config> c_main;
        std::shared_ptr<QPDFJob::CopyAttConfig> c_copy_att;
        std::shared_ptr<QPDFJob::AttConfig> c_att;
        std::shared_ptr<QPDFJob::PagesConfig> c_pages;
        std::shared_ptr<QPDFJob::UOConfig> c_uo;
        std::shared_ptr<QPDFJob::EncConfig> c_enc;
    };
}

Handlers::Handlers(bool partial, std::shared_ptr<QPDFJob::Config> c_main) :
    partial(partial),
    jh(nullptr),
    c_main(c_main)
{
    initHandlers();
}

void
Handlers::usage(std::string const& message)
{
    throw QPDFUsage(message);
}

Handlers::bare_handler_t
Handlers::bindBare(void (Handlers::*f)())
{
    return std::bind(std::mem_fn(f), this);
}

Handlers::json_handler_t
Handlers::bindJSON(void (Handlers::*f)(JSON))
{
    return std::bind(std::mem_fn(f), this, std::placeholders::_1);
}

void
Handlers::initHandlers()
{
    this->json_handlers.push_back(std::make_shared<JSONHandler>());
    this->jh = this->json_handlers.back().get();
    jh->addDictHandlers(
        [](std::string const&, JSON){},
        [this](std::string const&){
            if (! this->partial)
            {
                c_main->checkConfiguration();
            }
        });

#   include <qpdf/auto_job_json_init.hh>

    if (this->json_handlers.size() != 1)
    {
        throw std::logic_error("QPDFJob_json: json_handlers size != 1 at end");
    }
}

void
Handlers::addBare(bare_handler_t fn)
{
    jh->addStringHandler(
        [this, fn](std::string const& path, std::string const& parameter){
        if (! parameter.empty())
        {
            QTC::TC("qpdf", "QPDFJob json bare not empty");
            usage(path + ": value must be the empty string");
        }
        else
        {
            fn();
        }
    });
}

void
Handlers::addParameter(param_handler_t fn)
{
    jh->addStringHandler(
        [fn](std::string const& path, std::string const& parameter){
            fn(parameter.c_str());
        });
}

void
Handlers::addChoices(char const** choices, bool required, param_handler_t fn)
{
    jh->addStringHandler(
        [fn, choices, required, this](
            std::string const& path, std::string const& parameter){

            char const* p = parameter.c_str();
            bool matches = false;
            if ((! required) && (parameter.empty()))
            {
                matches = true;
            }
            if (! matches)
            {
                for (char const** i = choices; *i; ++i)
                {
                    if (strcmp(*i, p) == 0)
                    {
                        QTC::TC("qpdf", "QPDFJob json choice match");
                        matches = true;
                        break;
                    }
                }
            }
            if (! matches)
            {
                QTC::TC("qpdf", "QPDFJob json choice mismatch");
                std::ostringstream msg;
                msg << path + ": unexpected value; expected one of ";
                bool first = true;
                for (char const** i = choices; *i; ++i)
                {
                    if (first)
                    {
                        first = false;
                    }
                    else
                    {
                        msg << ", ";
                    }
                    msg << *i;
                }
                usage(msg.str());
            }
            fn(parameter.c_str());
        });
}

void
Handlers::pushKey(std::string const& key)
{
    auto new_jh = std::make_shared<JSONHandler>();
    this->jh->addDictKeyHandler(key, new_jh);
    this->json_handlers.push_back(new_jh);
    this->jh = new_jh.get();
}

void
Handlers::beginDict(json_handler_t start_fn, bare_handler_t end_fn)
{
    jh->addDictHandlers(
        [start_fn](std::string const&, JSON j){ start_fn(j); },
        [end_fn](std::string const&){ end_fn(); });
}

void
Handlers::beginArray(json_handler_t start_fn, bare_handler_t end_fn)
{
    auto item_jh = std::make_shared<JSONHandler>();
    jh->addArrayHandlers(
        [start_fn](std::string const&, JSON j){ start_fn(j); },
        [end_fn](std::string const&){ end_fn(); },
        item_jh);
    this->json_handlers.push_back(item_jh);
    this->jh = item_jh.get();
}

void
Handlers::ignoreItem()
{
    jh->addAnyHandler([](std::string const&, JSON){});
}

void
Handlers::popHandler()
{
    this->json_handlers.pop_back();
    this->jh = this->json_handlers.back().get();
}

void
Handlers::handle(JSON& j)
{
    this->json_handlers.back()->handle(".", j);
}

void
Handlers::setupInputFile()
{
    addParameter([this](char const* p) {
        c_main->inputFile(p);
    });
}

void
Handlers::setupPassword()
{
    addParameter([this](char const* p) {
        c_main->password(p);
    });
}

void
Handlers::setupEmpty()
{
    addBare([this]() {
        c_main->emptyInput();
    });
}

void
Handlers::setupOutputFile()
{
    addParameter([this](char const* p) {
        c_main->outputFile(p);
    });
}

void
Handlers::setupReplaceInput()
{
    addBare([this]() {
        c_main->replaceInput();
    });
}

void
Handlers::beginEncrypt(JSON j)
{
    // This method is only called if the overall JSON structure
    // matches the schema, so we already know that keys that are
    // present have the right types.
    int key_len = 0;
    std::string user_password;
    std::string owner_password;
    bool user_password_seen = false;
    bool owner_password_seen = false;
    j.forEachDictItem([&](std::string const& key, JSON value){
        if ((key == "40bit") || (key == "128bit") || (key == "256bit"))
        {
            if (key_len != 0)
            {
                QTC::TC("qpdf", "QPDFJob json encrypt duplicate key length");
                usage("exactly one of 40bit, 128bit, or 256bit must be given");
            }
            key_len = QUtil::string_to_int(key.c_str());
        }
        else if (key == "userPassword")
        {
            user_password_seen = value.getString(user_password);
        }
        else if (key == "ownerPassword")
        {
            owner_password_seen = value.getString(owner_password);
        }
    });
    if (key_len == 0)
    {
        QTC::TC("qpdf", "QPDFJob json encrypt no key length");
        usage("exactly one of 40bit, 128bit, or 256bit must be given;"
              " an empty dictionary may be supplied for one of them"
              " to set the key length without imposing any restrictions");
    }
    if (! (user_password_seen && owner_password_seen))
    {
        QTC::TC("qpdf", "QPDFJob json encrypt missing password");
        usage("the user and owner password are both required; use the empty"
              " string for the user password if you don't want a password");
    }
    this->c_enc = c_main->encrypt(key_len, user_password, owner_password);
}

void
Handlers::endEncrypt()
{
    this->c_enc->endEncrypt();
    this->c_enc = nullptr;
}

void
Handlers::setupEncryptUserPassword()
{
    // handled in beginEncrypt
    ignoreItem();
}

void
Handlers::setupEncryptOwnerPassword()
{
    // handled in beginEncrypt
    ignoreItem();
}

void
Handlers::beginEncrypt40bit(JSON)
{
    // nothing needed
}

void
Handlers::endEncrypt40bit()
{
    // nothing needed
}

void
Handlers::beginEncrypt128bit(JSON)
{
    // nothing needed
}

void
Handlers::endEncrypt128bit()
{
    // nothing needed
}

void
Handlers::beginEncrypt256bit(JSON)
{
    // nothing needed
}

void
Handlers::endEncrypt256bit()
{
    // nothing needed
}

void
Handlers::beginJsonKeyArray(JSON)
{
    // nothing needed
}

void
Handlers::endJsonKeyArray()
{
    // nothing needed
}

void
Handlers::beginJsonObjectArray(JSON)
{
    // nothing needed
}

void
Handlers::endJsonObjectArray()
{
    // nothing needed
}

void
Handlers::beginAddAttachmentArray(JSON)
{
    // nothing needed
}

void
Handlers::endAddAttachmentArray()
{
    // nothing needed
}

void
Handlers::beginAddAttachment(JSON)
{
    this->c_att = c_main->addAttachment();
}

void
Handlers::endAddAttachment()
{
    this->c_att->endAddAttachment();
    this->c_att = nullptr;
}

void
Handlers::setupAddAttachmentFile()
{
    addParameter([this](char const* p) {
        c_att->file(p);
    });
}

void
Handlers::beginCopyAttachmentsFromArray(JSON)
{
    // nothing needed
}

void
Handlers::endCopyAttachmentsFromArray()
{
    // nothing needed
}

void
Handlers::beginCopyAttachmentsFrom(JSON)
{
    this->c_copy_att = c_main->copyAttachmentsFrom();
}

void
Handlers::endCopyAttachmentsFrom()
{
    this->c_copy_att->endCopyAttachmentsFrom();
    this->c_copy_att = nullptr;
}

void
Handlers::setupCopyAttachmentsFromFile()
{
    addParameter([this](char const* p) {
        c_copy_att->file(p);
    });
}

void
Handlers::setupCopyAttachmentsFromPassword()
{
    addParameter([this](char const* p) {
        c_copy_att->password(p);
    });
}

void
Handlers::beginPagesArray(JSON)
{
    this->c_pages = c_main->pages();
}

void
Handlers::endPagesArray()
{
    c_pages->endPages();
    c_pages = nullptr;
}

void
Handlers::beginPages(JSON j)
{
    std::string file;
    std::string range("1-z");
    std::string password;
    bool file_seen = false;
    bool password_seen = false;
    j.forEachDictItem([&](std::string const& key, JSON value){
        if (key == "file")
        {
            file_seen = value.getString(file);
        }
        else if (key == "range")
        {
            value.getString(range);
        }
        else if (key == "password")
        {
            password_seen = value.getString(password);
        }
    });
    if (! file_seen)
    {
        QTC::TC("qpdf", "QPDFJob json pages no file");
        usage("file is required in page specification");
    }
    this->c_pages->pageSpec(
        file, range, password_seen ? password.c_str() : nullptr);
}

void
Handlers::endPages()
{
    // nothing needed
}

void
Handlers::setupPagesFile()
{
    // handled in beginPages
    ignoreItem();
}

void
Handlers::setupPagesPassword()
{
    // handled in beginPages
    ignoreItem();
}

void
Handlers::setupPagesRange()
{
    // handled in beginPages
    ignoreItem();
}

void
Handlers::beginOverlay(JSON)
{
    this->c_uo = c_main->overlay();
}

void
Handlers::endOverlay()
{
    c_uo->endUnderlayOverlay();
    c_uo = nullptr;
}

void
Handlers::setupOverlayFile()
{
    addParameter([this](char const* p) {
        c_uo->file(p);
    });
}

void
Handlers::setupOverlayPassword()
{
    addParameter([this](char const* p) {
        c_uo->password(p);
    });
}

void
Handlers::beginUnderlay(JSON)
{
    this->c_uo = c_main->underlay();
}

void
Handlers::endUnderlay()
{
    c_uo->endUnderlayOverlay();
    c_uo = nullptr;
}

void
Handlers::setupUnderlayFile()
{
    addParameter([this](char const* p) {
        c_uo->file(p);
    });
}

void
Handlers::setupUnderlayPassword()
{
    addParameter([this](char const* p) {
        c_uo->password(p);
    });
}

void
QPDFJob::initializeFromJson(std::string const& json, bool partial)
{
    std::list<std::string> errors;
    JSON j = JSON::parse(json);
    if (! j.checkSchema(JOB_SCHEMA, JSON::f_optional, errors))
    {
        std::ostringstream msg;
        msg << this->m->message_prefix
            << ": job json has errors:";
        for (auto const& error: errors)
        {
            msg << std::endl << "  " << error;
        }
        throw std::runtime_error(msg.str());
    }

    Handlers(partial, config()).handle(j);
}