#include "Machine.h"

#include <condition_variable>

#include <DDBusSender>

#include "Manager.h"
#include "MachineDBusAdaptor.h"
#include "ClipboardBase.h"
#include "Wrappers/InputEmittorWrapper.h"
#include "Wrappers/ConfirmDialogWrapper.h"
#include "Fuse/FuseServer.h"
#include "Fuse/FuseClient.h"
#include "utils/message_helper.h"

#include "protocol/message.pb.h"

#include "uvxx/TCP.h"
#include "uvxx/Loop.h"
#include "uvxx/Timer.h"
#include "uvxx/Addr.h"
#include "uvxx/Async.h"
#include "uvxx/Process.h"

#include "utils/net.h"

namespace fs = std::filesystem;

static const std::string fileSchema{"file://"};
static const std::string clipboardFileTarget{"x-special/gnome-copied-files"};
static const std::string uriListTarget{"text/uri-list"};

static const uint64_t U10s = 10 * 1000;
static const uint64_t U25s = 25 * 1000;

Machine::Machine(Manager *manager,
                 ClipboardBase *clipboard,
                 const std::shared_ptr<uvxx::Loop> &uvLoop,
                 QDBusConnection bus,
                 uint32_t id,
                 const fs::path &dataDir,
                 const std::string &ip,
                 uint16_t port,
                 const DeviceInfo &sp)
    : m_bus(bus)
    , m_manager(manager)
    , m_dbusAdaptor(new MachineDBusAdaptor(m_manager, this, m_bus, uvLoop))
    , m_clipboard(clipboard)
    , m_dataDir(dataDir)
    , m_mountpoint(m_dataDir / "mp")
    , m_path(QString("/org/deepin/dde/Cooperation1/Machine/%1").arg(id))
    , m_port(port)
    , m_uuid(sp.uuid())
    , m_name(sp.name())
    , m_connected(false)
    , m_os(sp.os())
    , m_compositor(sp.compositor())
    , m_deviceSharing(false)
    , m_direction(FLOW_DIRECTION_RIGHT)
    , m_pingTimer(std::make_shared<uvxx::Timer>(uvLoop, uvxx::memFunc(this, &Machine::ping)))
    , m_offlineTimer(
          std::make_shared<uvxx::Timer>(uvLoop, uvxx::memFunc(this, &Machine::onOffline)))
    , m_mounted(false)
    , m_uvLoop(uvLoop)
    , m_async(std::make_shared<uvxx::Async>(uvLoop))
    , m_ip(ip) {

    QDBusConnection::sessionBus();

    m_inputEmittors.emplace(
        std::make_pair(InputDeviceType::KEYBOARD,
                       std::make_unique<InputEmittorWrapper>(weak_from_this(),
                                                             m_uvLoop,
                                                             InputDeviceType::KEYBOARD)));
    m_inputEmittors.emplace(std::make_pair(
        InputDeviceType::MOUSE,
        std::make_unique<InputEmittorWrapper>(weak_from_this(), m_uvLoop, InputDeviceType::MOUSE)));
    m_inputEmittors.emplace(
        std::make_pair(InputDeviceType::TOUCHPAD,
                       std::make_unique<InputEmittorWrapper>(weak_from_this(),
                                                             m_uvLoop,
                                                             InputDeviceType::TOUCHPAD)));

    m_pingTimer->start(U10s);
    m_offlineTimer->oneshot(U25s);

    m_bus.registerObject(m_path, this);
    m_dbusAdaptor->updateUUID(QString::fromStdString(m_uuid));
}

Machine::~Machine() {
    m_pingTimer->close();
    m_offlineTimer->close();
    m_async->close();
    if (m_conn) {
        m_conn->onClosed(nullptr);
        m_conn->close();
        m_manager->onStopDeviceSharing();
    }

    m_bus.unregisterObject(m_path);
}

bool Machine::isPcMachine() const {
    return m_os == DEVICE_OS_UOS || m_os == DEVICE_OS_LINUX || m_os == DEVICE_OS_WINDOWS ||
           m_os == DEVICE_OS_MACOS;
}

bool Machine::isAndroid() const {
    return m_os == DEVICE_OS_ANDROID;
}

void Machine::connect() {
    m_conn = std::make_shared<uvxx::TCP>(m_uvLoop);

    m_conn->onConnected([this]() {
        spdlog::info("connected");

        initConnection();
        m_conn->startRead();

        m_pingTimer->stop();
        m_offlineTimer->stop();

        Message msg;
        auto *request = msg.mutable_pairrequest();
        request->set_key(SCAN_KEY);
        request->mutable_deviceinfo()->set_uuid(m_manager->uuid());
        request->mutable_deviceinfo()->set_name(Net::getHostname());
        request->mutable_deviceinfo()->set_os(DEVICE_OS_LINUX);
        request->mutable_deviceinfo()->set_compositor(COMPOSITOR_X11);

        sendMessage(msg);
    });
    m_conn->onConnectFailed(
        [this]([[maybe_unused]] const std::string &title, const std::string &msg) {
            spdlog::info("connect failed: {}", msg);

            // TODO tips and send scan
            m_manager->ping(m_ip);
        });
    m_conn->connect(uvxx::IPv4Addr::create(m_ip, m_port));
}

void Machine::updateMachineInfo(const std::string &ip, uint16_t port, const DeviceInfo &devInfo) {
    m_ip = ip;
    m_port = port;
    m_name = devInfo.name();
    m_compositor = devInfo.compositor();
}

void Machine::receivedPing() {
    m_offlineTimer->reset();
    m_pingTimer->reset();
}

void Machine::onPair(const std::shared_ptr<uvxx::TCP> &sock) {
    spdlog::info("request onPair");
    m_conn = sock;

    m_confirmDialog = std::make_unique<ConfirmDialogWrapper>(
        m_ip,
        m_name,
        m_uvLoop,
        uvxx::memFunc(this, &Machine::receivedUserConfirm));
}

void Machine::disconnect() {
    m_conn->close();
}

void Machine::requestDeviceSharing() {
    Message msg;
    msg.mutable_devicesharingstartrequest();
    sendMessage(msg);
}

void Machine::stopDeviceSharing() {
    Message msg;
    msg.mutable_devicesharingstoprequest();
    sendMessage(msg);

    stopDeviceSharingAux();
}

void Machine::setFlowDirection(FlowDirection direction) {
    if (m_direction != direction) {
        m_direction = (FlowDirection)direction;
        sendFlowDirectionNtf();
    }
}

void Machine::ping() {
    m_manager->ping(m_ip);
}

void Machine::onOffline() {
    m_manager->onMachineOffline(m_uuid);
}

void Machine::initConnection() {
    m_conn->onClosed(uvxx::memFunc(this, &Machine::handleDisconnectedAux));
    m_conn->onReceived(uvxx::memFunc(this, &Machine::dispatcher));
    m_conn->tcpNoDelay();
    m_conn->keepalive(true, 20);
}

void Machine::handleDisconnectedAux() {
    spdlog::info("disconnected");

    if (m_connected) {
        m_manager->onStopDeviceSharing();

        m_deviceSharing = false;
        m_dbusAdaptor->updateDeviceSharing(m_deviceSharing);
        m_connected = false;
        m_dbusAdaptor->updateConnected(m_connected);
    }

    if (m_fuseClient) {
        m_fuseClient->exit();
        m_fuseClient.reset();
    }

    if (m_fuseServer) {
        m_fuseServer.reset();
    }

    m_conn.reset();

    m_pingTimer->reset();

    handleDisconnected();
}

void Machine::dispatcher(uvxx::Buffer &buff) noexcept {
    spdlog::info("received packet from name: {}, UUID: {}, size: {}",
                 std::string(m_name),
                 std::string(m_uuid),
                 buff.size());

    while (buff.size() >= header_size) {
        auto res = MessageHelper::parseMessage<Message>(buff);
        if (!res.has_value()) {
            if (res.error() == MessageHelper::PARSE_ERROR::ILLEGAL_MESSAGE) {
                spdlog::error("illegal message from {}, close the connection", std::string(m_uuid));
                m_conn->close();
            }
            return;
        }

        Message &msg = res.value();
        spdlog::debug("message type: {}", msg.payload_case());

        switch (msg.payload_case()) {
        case Message::PayloadCase::kPairResponse: {
            handlePairResponseAux(msg.pairresponse());
            break;
        }

        case Message::PayloadCase::kServiceOnOffNotification: {
            handleServiceOnOffMsg(msg.serviceonoffnotification());
            break;
        }

        case Message::PayloadCase::kDeviceSharingStartRequest: {
            handleDeviceSharingStartRequest();
            break;
        }

        case Message::PayloadCase::kDeviceSharingStartResponse: {
            handleDeviceSharingStartResponse(msg.devicesharingstartresponse());
            break;
        }

        case Message::PayloadCase::kDeviceSharingStopRequest: {
            handleDeviceSharingStopRequest();
            break;
        }

        case Message::PayloadCase::kDeviceSharingStopResponse: {
            break;
        }

        case Message::PayloadCase::kInputEventRequest: {
            handleInputEventRequest(msg.inputeventrequest());
            break;
        }

        case Message::PayloadCase::kInputEventResponse: {
            break;
        }

        case Message::PayloadCase::kFlowDirectionNtf: {
            handleFlowDirectionNtf(msg.flowdirectionntf());
            break;
        }

        case Message::PayloadCase::kFlowRequest: {
            handleFlowRequest(msg.flowrequest());
            break;
        }

        case Message::PayloadCase::kFlowResponse: {
            break;
        }

        case Message::PayloadCase::kFsRequest: {
            handleFsRequest(msg.fsrequest());
            break;
        }

        case Message::PayloadCase::kFsResponse: {
            handleFsResponse(msg.fsresponse());
            break;
        }

        case Message::PayloadCase::kFsSendFileRequest: {
            handleFsSendFileRequest(msg.fssendfilerequest());
            break;
        }

        case Message::PayloadCase::kFsSendFileResponse: {
            break;
        }

        case Message::PayloadCase::kFsSendFileResult: {
            break;
        }

        case Message::PayloadCase::kClipboardNotify: {
            handleClipboardNotify(msg.clipboardnotify());
            break;
        }

        case Message::PayloadCase::kClipboardGetContentRequest: {
            handleClipboardGetContentRequest(msg.clipboardgetcontentrequest());
            break;
        }

        case Message::PayloadCase::kClipboardGetContentResponse: {
            handleClipboardGetContentResponse(msg.clipboardgetcontentresponse());
            break;
        }

        default: {
            spdlog::warn("invalid message type: {}", msg.payload_case());
            m_conn->close();
            return;
            break;
        }
        }
    }
}

void Machine::handlePairResponseAux(const PairResponse &resp) {
    bool agree = resp.agree();
    if (!agree) {
        // handle not agree
        m_conn->close();
        // rejected, need notify,ui can reset connecting status
        m_connected = false;
        m_dbusAdaptor->updateConnected(m_connected);
        return;
    }

    m_connected = true;
    m_dbusAdaptor->updateConnected(m_connected);

    sendServiceStatusNotification();
    handleConnected();
}

void Machine::handleServiceOnOffMsg(const ServiceOnOffNotification &notification) {
    m_sharedClipboard = notification.sharedclipboardon();
}

void Machine::handleDeviceSharingStartRequest() {
    bool accepted = true;
    m_async->wake([this, accepted]() {
        Message msg;
        DeviceSharingStartResponse *resp = msg.mutable_devicesharingstartresponse();
        resp->set_accept(accepted);
        sendMessage(msg);

        if (accepted) {
            auto wptr = weak_from_this();
            m_manager->onStartDeviceSharing(wptr, true);

            m_deviceSharing = true;
            m_dbusAdaptor->updateDeviceSharing(m_deviceSharing);

            m_manager->machineCooperated(m_uuid);

            m_direction = FLOW_DIRECTION_LEFT;
            m_dbusAdaptor->updateDirection(m_direction);
        }
    });
}

void Machine::handleDeviceSharingStartResponse(const DeviceSharingStartResponse &resp) {
    if (!resp.accept()) {
        return;
    }

    m_deviceSharing = true;
    m_dbusAdaptor->updateDeviceSharing(m_deviceSharing);

    m_manager->machineCooperated(m_uuid);

    m_direction = FLOW_DIRECTION_RIGHT;
    m_dbusAdaptor->updateDirection(m_direction);

    sendFlowDirectionNtf();

    m_manager->onStartDeviceSharing(weak_from_this(), true);
}

void Machine::handleDeviceSharingStopRequest() {
    stopDeviceSharingAux();
}

void Machine::handleInputEventRequest(const InputEventRequest &req) {
    spdlog::debug("received input event");

    bool success = true;

    auto deviceType = static_cast<InputDeviceType>(req.devicetype());
    auto it = m_inputEmittors.find(deviceType);
    if (it == m_inputEmittors.end()) {
        success = false;
        spdlog::error("no deviceType {} found", static_cast<uint8_t>(deviceType));
    } else {
        auto &inputEmittor = it->second;
        success = inputEmittor->emitEvent(req.type(), req.code(), req.value());
    }

    Message resp;
    InputEventResponse *response = resp.mutable_inputeventresponse();
    response->set_serial(req.serial());
    response->set_success(success);

    m_conn->write(MessageHelper::genMessage(resp));
}

void Machine::handleFlowDirectionNtf(const FlowDirectionNtf &ntf) {
    FlowDirection peerFlowDirection = ntf.direction();
    switch ((int)peerFlowDirection) {
    case FLOW_DIRECTION_TOP:
        m_direction = FLOW_DIRECTION_BOTTOM;
        break;
    case FLOW_DIRECTION_BOTTOM:
        m_direction = FLOW_DIRECTION_TOP;
        break;
    case FLOW_DIRECTION_LEFT:
        m_direction = FLOW_DIRECTION_RIGHT;
        break;
    case FLOW_DIRECTION_RIGHT:
        m_direction = FLOW_DIRECTION_LEFT;
        break;
    }
}

void Machine::handleFlowRequest(const FlowRequest &req) {
    m_manager->onFlowBack(req.direction(), req.x(), req.y());
}

void Machine::handleFsRequest([[maybe_unused]] const FsRequest &req) {
    if (m_fuseServer) {
        Message msg;
        auto *fsresponse = msg.mutable_fsresponse();
        fsresponse->set_accepted(false);
        fsresponse->set_port(0);
        sendMessage(msg);
        return;
    }

    m_fuseServer = std::make_unique<FuseServer>(weak_from_this(), m_uvLoop);

    // TODO: request accept
    Message msg;
    auto *fsresponse = msg.mutable_fsresponse();
    fsresponse->set_accepted(true);
    fsresponse->set_port(m_fuseServer->port());
    sendMessage(msg);
}

void Machine::handleFsResponse(const FsResponse &resp) {
    if (!resp.accepted()) {
        return;
    }

    m_fuseClient = std::make_unique<FuseClient>(m_uvLoop, m_ip, resp.port(), m_mountpoint);
}

void Machine::handleFsSendFileRequest(const FsSendFileRequest &req) {
    Message msg;
    auto *fssendfileresponse = msg.mutable_fssendfileresponse();
    fssendfileresponse->set_serial(req.serial());

    if (!m_fuseClient) {
        fssendfileresponse->set_accepted(false);
        sendMessage(msg);
        return;
    }

    fssendfileresponse->set_accepted(true);
    sendMessage(msg);

    QString storagePath = m_manager->fileStoragePath();
    std::string reqPath = req.path();
    if (!reqPath.empty() && reqPath[0] != '/') {
        reqPath = "/" + reqPath;
    }
    std::string filePath = m_mountpoint.string() + reqPath;
    auto process = std::make_shared<uvxx::Process>(m_uvLoop, "/bin/cp");
    process->onExit([this, storagePath = storagePath.toStdString(), serial = req.serial(), path = req.path(), process](
                        int64_t exit_status,
                        [[maybe_unused]] int term_signal) {
        Message msg;
        auto *fssendfileresult = msg.mutable_fssendfileresult();
        fssendfileresult->set_serial(serial);
        fssendfileresult->set_path(path);

        if (exit_status != 0) {
            spdlog::info("copy files failed");
        } else {
            spdlog::info("copy files success");
        }

        std::string::size_type iPos = path.find_last_of('/') + 1;
        std::string fileName = path.substr(iPos, path.length() - iPos);
        sendReceivedFilesSystemNtf(storagePath + "/" + fileName, exit_status == 0);

        fssendfileresult->set_result(exit_status == 0);
        sendMessage(msg);

        process->onExit(nullptr);
    });
    process->spawn();
}

void Machine::handleClipboardNotify(const ClipboardNotify &notify) {
    auto &targetsp = notify.targets();
    std::vector<std::string> targets{targetsp.cbegin(), targetsp.cend()};

    // other pc machine need fill up text/uri-list target
    if (m_os != DEVICE_OS_UOS &&
        std::find(targets.begin(), targets.end(), clipboardFileTarget) != targets.end()) {
        targets.emplace_back(uriListTarget);
    }

    m_manager->onMachineOwnClipboard(weak_from_this(), targets);
}

void Machine::handleClipboardGetContentRequest(const ClipboardGetContentRequest &req) {
    auto target = req.target();
    auto cb = [this, target](const std::vector<char> &content) {
        Message msg;
        auto *reply = msg.mutable_clipboardgetcontentresponse();
        reply->set_target(target);
        reply->set_content(std::string(content.begin(), content.end()));
        sendMessage(msg);
    };
    m_clipboard->readTargetContent(target, cb);
}

void Machine::handleClipboardGetContentResponse(const ClipboardGetContentResponse &resp) {
    auto target = resp.target();
    auto content = resp.content();
    if (target == "x-special/gnome-copied-files") {
        spdlog::warn("ori x-special/gnome-copied-files: {}", content);
    }
    if (m_clipboard->isFiles()) {
        std::stringstream ss(content);
        std::string out;
        std::string line;
        while (!ss.eof()) {
            std::getline(ss, line, '\n');
            if (line[0] == '/') { // starts with '/'
                out.reserve(out.length() + m_mountpoint.string().length() + line.length() + 1);
                out.append(m_mountpoint.string());
                out.append(line);
            } else if (line.rfind(fileSchema, 0) == 0) { // starts with 'file://'
                out.reserve(out.length() + m_mountpoint.string().length() + line.length() + 1);
                out.append(fileSchema);
                out.append(m_mountpoint.string());
                out.append(line.begin() + fileSchema.length(), line.end());
            } else {
                out.reserve(out.length() + line.length() + 1);
                out.append(line);
            }
            out.push_back('\n');
        }
        out.resize(out.length() - 1);
        content.swap(out);
        // spdlog::info("content[{}]: {}", content.length(), content);
    }

    // fill up text/uri-list target. when pasted, this target is need;
    if (m_os != DEVICE_OS_UOS && target == clipboardFileTarget) {
        std::stringstream tempStream(content);
        std::string filePath;
        while (!tempStream.eof()) {
            std::getline(tempStream, filePath, '\n');
            if (!filePath.empty() && filePath.rfind(fileSchema, 0) == 0) { // starts with 'file://'
                filePath = std::string(filePath.data() + fileSchema.size(),
                                       filePath.size() - fileSchema.size());
                break;
            }
        }

        if (!filePath.empty()) {
            spdlog::info("pc machine fill up text/uri-list target:{}", filePath);
            m_clipboard->updateTargetContent(uriListTarget,
                                             std::vector<char>(filePath.begin(), filePath.end()));
        }
    }

    m_clipboard->updateTargetContent(target, std::vector<char>(content.begin(), content.end()));
}

void Machine::onInputGrabberEvent(uint8_t deviceType,
                                  unsigned int type,
                                  unsigned int code,
                                  int value) {
    Message msg;
    auto *inputEvent = msg.mutable_inputeventrequest();
    inputEvent->set_devicetype(static_cast<DeviceType>(deviceType));
    inputEvent->set_type(type);
    inputEvent->set_code(code);
    inputEvent->set_value(value);
    sendMessage(msg);
}

void Machine::onClipboardTargetsChanged(const std::vector<std::string> &targets) {
    // sharedClipboard off, not send msg
    if (!m_manager->isSharedClipboard()) {
        return;
    }

    Message msg;
    auto *clipboardNotify = msg.mutable_clipboardnotify();
    *(clipboardNotify->mutable_targets()) = {targets.cbegin(), targets.cend()};
    sendMessage(msg);
}

void Machine::flowTo(uint16_t direction, uint16_t x, uint16_t y) noexcept {
    Message msg;
    FlowRequest *flow = msg.mutable_flowrequest();
    flow->set_direction(FlowDirection(direction));
    flow->set_x(x);
    flow->set_y(y);
    sendMessage(msg);
}

void Machine::readTarget(const std::string &target) {
    Message msg;
    auto *clipboardGetContent = msg.mutable_clipboardgetcontentrequest();
    clipboardGetContent->set_target(target);
    sendMessage(msg);
}

void Machine::stopDeviceSharingAux() {
    m_manager->onStopDeviceSharing();

    m_deviceSharing = false;
    m_dbusAdaptor->updateDeviceSharing(m_deviceSharing);
}

void Machine::receivedUserConfirm(uvxx::Buffer &buff) {
    m_confirmDialog.reset();

    if (buff.size() != 1) {
        spdlog::warn("user confirm has error!");
        return;
    }

    bool isAccept = (buff.data()[0] == ACCEPT);
    buff.clear();

    Message msg;
    auto *response = msg.mutable_pairresponse();
    response->set_key(SCAN_KEY);
    response->mutable_deviceinfo()->set_uuid(m_manager->uuid());
    response->mutable_deviceinfo()->set_name(Net::getHostname());
    response->mutable_deviceinfo()->set_os(DEVICE_OS_LINUX);
    response->mutable_deviceinfo()->set_compositor(COMPOSITOR_X11);
    response->set_agree(isAccept); // 询问用户是否同意

    sendMessage(msg);

    if (isAccept) {
        initConnection();

        m_pingTimer->stop();
        m_offlineTimer->stop();

        m_connected = true;
        m_dbusAdaptor->updateConnected(m_connected);

        sendServiceStatusNotification();
        handleConnected();
    } else {
        m_conn->close();
    }
}

void Machine::sendFlowDirectionNtf() {
    Message msg;
    auto *notification = msg.mutable_flowdirectionntf();
    notification->set_direction((FlowDirection)m_direction);
    sendMessage(msg);
}

void Machine::sendReceivedFilesSystemNtf(const std::string &path, bool isSuccess) {
    DDBusSender()
        .service("org.freedesktop.Notifications")
        .path("/org/freedesktop/Notifications")
        .interface("org.freedesktop.Notifications")
        .method(QString("Notify"))
        .arg(QObject::tr("collaboration"))
        .arg(static_cast<uint>(0))
        .arg(QString(""))
        .arg(QString(""))
        .arg(QString(QObject::tr("Receive file %1 %2"))
                 .arg(QString::fromStdString(path))
                 .arg(isSuccess ? "success" : "failed"))
        .arg(QStringList())
        .arg(QVariantMap())
        .arg(5000)
        .call();
}

void Machine::sendFiles(const QStringList &filePaths) {
    m_async->wake([this, filePaths]() {
        for (const QString &filePath : filePaths) {
            Message msg;
            FsSendFileRequest *send = msg.mutable_fssendfilerequest();
            send->set_path(filePath.toStdString());
            sendMessage(msg);
        }
    });
}

void Machine::sendMessage(const Message &msg) {
    if (!m_conn) {
        spdlog::warn("connection reset but still want to send msg:{}", msg.GetTypeName());
        return;
    }

    m_conn->write(MessageHelper::genMessage(msg));
}

void Machine::sendServiceStatusNotification() {
    Message msg;
    auto *notification = msg.mutable_serviceonoffnotification();
    notification->set_sharedclipboardon(m_manager->isSharedClipboard());
    notification->set_shareddeviceson(m_manager->isSharedDevices());

    sendMessage(msg);
}