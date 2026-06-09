import 'dart:async';
import 'dart:convert';
import 'dart:math';
import 'dart:typed_data';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../utils/constants.dart';

class BleService {
  BluetoothDevice? _device;
  BluetoothCharacteristic? _notify;
  BluetoothCharacteristic? _write;
  final _jsonStreamController = StreamController<String>.broadcast();
  final _connectionStateController = StreamController<bool>.broadcast();
  StreamSubscription<BluetoothConnectionState>? _connectionStateSub;
  bool _isReconnecting = false;
  int _reconnectAttempts = 0;
  static const _maxReconnectAttempts = 5;

  Stream<String> get jsonStream => _jsonStreamController.stream;
  Stream<bool> get connectionStateStream => _connectionStateController.stream;
  BluetoothDevice? get connectedDevice => _device;
  String? _savedDeviceName;
  String? get connectedName => _device?.platformName.isNotEmpty == true ? _device?.platformName : _savedDeviceName;
  String? get connectedId => _device?.remoteId.str;
  bool get isReconnecting => _isReconnecting;

  static const _lastDeviceKey = "last_ble_device";
  static const _lastDeviceNameKey = "last_ble_device_name";

  /// ⏱️ Scanner rapide
  Future<List<BluetoothDevice>> scanDevices({Duration timeout = const Duration(seconds: 2)}) async {
    final List<BluetoothDevice> found = [];

    await FlutterBluePlus.startScan(timeout: timeout);
    final sub = FlutterBluePlus.scanResults.listen((results) {
      for (final r in results) {
        if (r.device.platformName.isNotEmpty) {
          if (!found.any((d) => d.remoteId == r.device.remoteId)) {
            found.add(r.device);
          }
        }
      }
    });

    await Future.delayed(timeout);
    await FlutterBluePlus.stopScan();
    await sub.cancel();
    return found;
  }

  /// 💾 Sauvegarde de l'ID et du nom du périphérique
  Future<void> saveDeviceId(String id, String name) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_lastDeviceKey, id);
    await prefs.setString(_lastDeviceNameKey, name);
  }

  /// 🔄 Récupération de l'ID du périphérique
  Future<BluetoothDevice?> getSavedDevice() async {
    final prefs = await SharedPreferences.getInstance();
    final id = prefs.getString(_lastDeviceKey);
    final name = prefs.getString(_lastDeviceNameKey);
    if (id != null) {
      _savedDeviceName = name; // Charger le nom sauvegardé
      return BluetoothDevice.fromId(id);
    }
    return null;
  }

  /// ⚡ Connexion rapide (sans rescan)
  Future<void> reconnectSavedDevice() async {
    final device = await getSavedDevice();
    if (device != null) {
      try {
        await connectTo(device);
        print("✅ Reconnexion rapide réussie à ${device.remoteId}");
      } catch (e) {
        print("❌ Reconnexion rapide échouée : $e");
      }
    }
  }

  /// 📡 Connexion à un périphérique
  Future<void> connectTo(BluetoothDevice device) async {
    await device.connect(autoConnect: false).catchError((_) {});
    _device = device;
    // Ne réinitialiser le compteur que si on n'est pas en train de se reconnecter
    if (!_isReconnecting) {
      _reconnectAttempts = 0;
    }
    // Ne sauvegarder que si on a un nom valide, sinon garder le nom existant en cache
    if (device.platformName.isNotEmpty) {
      await saveDeviceId(device.remoteId.str, device.platformName);
      _savedDeviceName = device.platformName; // Mettre à jour le cache
    }

    // Écouter les changements d'état de connexion pour détecter les déconnexions
    await _connectionStateSub?.cancel();
    _connectionStateSub = device.connectionState.listen((state) {
      if (state == BluetoothConnectionState.disconnected) {
        print("! Déconnexion BLE détectée");
        _connectionStateController.add(false);
        if (!_isReconnecting) {
          _onDeviceDisconnected();
        }
      } else if (state == BluetoothConnectionState.connected) {
        _connectionStateController.add(true);
      }
    });

    final services = await device.discoverServices();

    for (final s in services) {
      for (final c in s.characteristics) {
        final id = c.uuid.toString().toLowerCase();
        if (id == BleUUID.notifyChar) _notify = c;
        if (id == BleUUID.writeChar) _write = c;
      }
    }

    if (_notify == null || _write == null) {
      for (final s in services) {
        for (final c in s.characteristics) {
          if (_notify == null && c.properties.notify) _notify = c;
          if (_write == null && c.properties.write) _write = c;
        }
      }
    }

    if (_notify == null) throw Exception("Aucune caractéristique notify trouvée");

    await _notify!.setNotifyValue(true);
    _notify!.onValueReceived.listen((data) {
      try {
        final s = utf8.decode(data);
        _jsonStreamController.add(s);
      } catch (_) {}
    });

    _connectionStateController.add(true);
  }

  /// 🔄 Gestion de la déconnexion inattendue avec reconnexion automatique
  Future<void> _onDeviceDisconnected() async {
    if (_device == null || _isReconnecting) return;

    _isReconnecting = true;
    _reconnectAttempts = 0; // Réinitialiser le compteur au début de la séquence
    print("🔄 Tentative de reconnexion automatique...");

    while (_reconnectAttempts < _maxReconnectAttempts) {
      final delaySeconds = (_reconnectAttempts + 1) * 2; // Backoff exponentiel: 2s, 4s, 6s, 8s, 10s
      print("🔄 Tentative ${_reconnectAttempts + 1}/$_maxReconnectAttempts (attente ${delaySeconds}s)");
      await Future.delayed(Duration(seconds: delaySeconds));

      _reconnectAttempts++;

      try {
        final savedDevice = await getSavedDevice();
        if (savedDevice == null) {
          print("❌ Aucun périphérique sauvegardé");
          break;
        }

        await connectTo(savedDevice);
        print("✅ Reconnexion automatique réussie !");
        _isReconnecting = false;
        _reconnectAttempts = 0;
        return;
      } catch (e) {
        print("❌ Tentative $_reconnectAttempts échouée: $e");
      }
    }

    print("❌ Reconnexion automatique échouée après $_maxReconnectAttempts tentatives");
    _isReconnecting = false;
    _reconnectAttempts = 0;
    await disconnect();
  }

  /// 🔌 Déconnexion
  Future<void> disconnect() async {
    _isReconnecting = false;
    await _connectionStateSub?.cancel();
    _connectionStateSub = null;
    try {
      await _device?.disconnect();
    } catch (_) {}
    _device = null;
    _savedDeviceName = null; // Effacer le nom en cache
    _notify = null;
    _write = null;
    _connectionStateController.add(false);
  }

  Future<void> requestCoefficients() async {
    final cmd = utf8.encode(jsonEncode({"get_coeff": true}));
    await _write?.write(cmd, withoutResponse: false);
  }

  Future<void> sendUpdatedCoefficients(Map<String, dynamic> coeff) async {
    final payload = {"update_coeff": coeff};
    final bytes = utf8.encode(jsonEncode(payload));
    await _write?.write(bytes, withoutResponse: false);
  }

  Future<void> requestFirmwareInfo() async {
    final cmd = utf8.encode(jsonEncode({"get_info": true}));
    await _write?.write(cmd, withoutResponse: false);
  }

  // ===================== OTA firmware (protocole docs/ota.md) =====================

  /// MTU actuellement négocié avec le périphérique (23 = minimum BLE si inconnu).
  int get mtu => _device?.mtuNow ?? 23;

  /// 📦 Transfert OTA complet du firmware vers l'ESP32.
  ///
  /// Envoie `ota_begin`, streame [firmware] en chunks (write without response si
  /// possible) avec une fenêtre glissante bornée par les accusés `ota_progress`,
  /// puis attend `ota_success` (l'ESP vérifie le SHA-256, finalise et reboote seul).
  ///
  /// [onProgress] est appelé avec (octets envoyés, octets totaux).
  /// Lève [OtaException] avec un message utilisateur en cas d'échec.
  Future<void> performOtaTransfer({
    required Uint8List firmware,
    required String sha256Hex,
    required String espModel,
    required String fwVersion,
    void Function(int sentBytes, int totalBytes)? onProgress,
  }) async {
    final write = _write;
    if (write == null) throw OtaException("Aucune connexion BLE active");

    // File des événements ota_* émis par l'ESP, consommés séquentiellement
    final queue = _OtaEventQueue();
    final sub = jsonStream.listen((s) {
      try {
        final j = jsonDecode(s);
        if (j is Map<String, dynamic> &&
            (j['event'] as String? ?? '').startsWith('ota_')) {
          queue.add(j);
        }
      } catch (_) {}
    });

    try {
      // 1. ota_begin — chunk proposé selon le MTU négocié, l'ESP le borne et confirme
      final proposedChunk = max(20, min(mtu - 3, 480));
      await write.write(
        utf8.encode(jsonEncode({
          "cmd": "ota_begin",
          "size": firmware.length,
          "sha256": sha256Hex.toLowerCase(),
          "esp_model": espModel,
          "fw_version": fwVersion,
          "chunk_size": proposedChunk,
        })),
        withoutResponse: false,
      );

      final ready = await queue.next(
        const Duration(seconds: 10),
        "L'ESP ne répond pas à la demande de mise à jour — son firmware est "
        "peut-être trop ancien pour l'OTA (mise à jour par USB requise).",
      );
      if (ready['event'] == 'ota_error') throw OtaException(describeOtaError(ready));
      if (ready['event'] != 'ota_ready') {
        throw OtaException("Réponse inattendue de l'ESP : ${ready['event']}");
      }
      final chunkSize = (ready['chunk_size'] as num?)?.toInt() ?? 240;
      final windowBytes = (ready['window_bytes'] as num?)?.toInt() ?? 8192;
      final useWnr = write.properties.writeWithoutResponse;

      // 2. Chunks avec fenêtre glissante : jamais plus de windowBytes non confirmés en vol
      int sent = 0;
      int acked = 0;
      while (sent < firmware.length) {
        while (sent - acked >= windowBytes) {
          final ev = await queue.next(
            const Duration(seconds: 15),
            "Transfert interrompu : plus d'accusé de réception de l'ESP.",
          );
          if (ev['event'] == 'ota_error') throw OtaException(describeOtaError(ev));
          if (ev['event'] == 'ota_progress') {
            acked = (ev['bytes'] as num?)?.toInt() ?? acked;
          }
        }
        final end = min(sent + chunkSize, firmware.length);
        await write.write(firmware.sublist(sent, end), withoutResponse: useWnr);
        sent = end;
        onProgress?.call(sent, firmware.length);
      }

      // 3. Derniers accusés puis confirmation finale (SHA vérifié, reboot imminent)
      while (true) {
        final ev = await queue.next(
          const Duration(seconds: 30),
          "Pas de confirmation finale de l'ESP après l'envoi du firmware.",
        );
        if (ev['event'] == 'ota_success') return;
        if (ev['event'] == 'ota_error') throw OtaException(describeOtaError(ev));
        // ota_progress intermédiaires : rien à faire
      }
    } finally {
      await sub.cancel();
      queue.dispose();
    }
  }

  /// Message utilisateur pour un événement {"event":"ota_error","reason":...} de l'ESP.
  static String describeOtaError(Map<String, dynamic> ev) {
    final reason = ev['reason'] as String? ?? 'inconnu';
    switch (reason) {
      case 'model_mismatch':
        return "Le binaire ne correspond pas au modèle de la carte "
            "(attendu ${ev['expected']}, reçu ${ev['got']}). Mise à jour refusée.";
      case 'size_too_large':
        return "Le firmware est trop volumineux pour la partition OTA de la carte.";
      case 'sha_mismatch':
        return "Le firmware reçu par l'ESP est corrompu (empreinte SHA-256 invalide). Réessayez.";
      case 'timeout':
        return "L'ESP n'a plus reçu de données pendant le transfert (timeout).";
      case 'ble_disconnected':
        return "Connexion BLE perdue pendant le transfert.";
      case 'already_running':
        return "Une mise à jour est déjà en cours sur l'ESP.";
      case 'flash_write':
      case 'flash_end':
        return "Erreur d'écriture de la mémoire flash de l'ESP ($reason).";
      case 'no_memory':
        return "Mémoire insuffisante sur l'ESP pour démarrer la mise à jour.";
      case 'no_backup':
        return "Aucun firmware précédent disponible pour un retour arrière.";
      default:
        return "Erreur OTA côté ESP : $reason";
    }
  }
}

/// Erreur de mise à jour OTA, avec un message destiné à l'utilisateur.
class OtaException implements Exception {
  final String message;
  OtaException(this.message);
  @override
  String toString() => message;
}

/// Petite file FIFO d'événements avec attente bornée, sans dépendance externe.
class _OtaEventQueue {
  final _buffer = <Map<String, dynamic>>[];
  final _waiters = <Completer<Map<String, dynamic>>>[];
  bool _disposed = false;

  void add(Map<String, dynamic> event) {
    if (_disposed) return;
    if (_waiters.isNotEmpty) {
      _waiters.removeAt(0).complete(event);
    } else {
      _buffer.add(event);
    }
  }

  Future<Map<String, dynamic>> next(Duration timeout, String timeoutMessage) {
    if (_buffer.isNotEmpty) return Future.value(_buffer.removeAt(0));
    final c = Completer<Map<String, dynamic>>();
    _waiters.add(c);
    return c.future.timeout(timeout, onTimeout: () {
      _waiters.remove(c);
      throw OtaException(timeoutMessage);
    });
  }

  void dispose() {
    _disposed = true;
    _waiters.clear();
    _buffer.clear();
  }
}
