import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import '../models/capteur_data.dart';
import '../models/flow_meter_source.dart';
import '../models/firmware_info.dart';
import '../services/ble_service.dart';
import '../services/simulation_service.dart';
import '../services/flow_meter_config_service.dart';
import '../services/moving_average_filter.dart';
import '../utils/constants.dart';

class DataProvider with ChangeNotifier {
  final BleService ble;
  final SimulationService sim;
  final FlowMeterConfigService flowMeterConfigService;

  CapteurData data = CapteurData.zero();
  // Filtre de moyenne glissante pour stabiliser l'affichage des valeurs.
  final MovingAverageFilter _filter = MovingAverageFilter(window: 5);
  FlowMeterConfig flowMeterConfig = const FlowMeterConfig();
  FirmwareInfo firmwareInfo = FirmwareInfo.empty();
  String? lastJson;                   // trame brute la plus récente
  DateTime? lastTick;                 // dernière réception
  bool get isAlive =>
      lastTick != null && DateTime.now().difference(lastTick!) <= tickDuration;

  BluetoothDevice? get connectedDevice => ble.connectedDevice;
  String? get connectedDeviceName => ble.connectedName;

  StreamSubscription<String>? _bleSub;
  StreamSubscription<bool>? _connectionStateSub;
  bool _isReconnecting = false;
  bool get isReconnecting => _isReconnecting;

  // 🔔 Timer pour forcer un rafraîchissement de l'UI à l'expiration du délai vert
  Timer? _aliveTimer;

  // 📊 Suivi glissant des IDs pour détecter la perte de trames
  final List<int> _lastFrameIds = [];
  static const int _maxFrameHistory = 10;
  bool _hasPacketLoss = false;
  bool get hasPacketLoss => _hasPacketLoss;

  DataProvider({
    required this.ble,
    required this.sim,
    required this.flowMeterConfigService,
  });

  /// 🚀 Initialisation au démarrage : tente de se reconnecter au dernier appareil
  Future<void> initialize() async {
    // Charger la configuration des débitmètres
    flowMeterConfig = await flowMeterConfigService.loadConfig();
    print("✅ Configuration débitmètres chargée : ${flowMeterConfig.toJson()}");

    final savedDevice = await ble.getSavedDevice();
    if (savedDevice != null) {
      print("🔄 Tentative de reconnexion automatique au dernier appareil...");
      _isReconnecting = true;
      notifyListeners();

      try {
        await connect(savedDevice);
        print("✅ Reconnexion automatique au démarrage réussie !");
      } catch (e) {
        print("❌ Reconnexion automatique au démarrage échouée : $e");
        _isReconnecting = false;
        notifyListeners();
      }
    }
  }

  /// Met à jour la configuration d'un débitmètre et sauvegarde
  Future<void> updateFlowMeterSource(String key, FlowMeterSource source) async {
    switch (key) {
      case 'DG1':
        flowMeterConfig = flowMeterConfig.copyWith(dg1: source);
        break;
      case 'DD1':
        flowMeterConfig = flowMeterConfig.copyWith(dd1: source);
        break;
      case 'DG2':
        flowMeterConfig = flowMeterConfig.copyWith(dg2: source);
        break;
      case 'DD2':
        flowMeterConfig = flowMeterConfig.copyWith(dd2: source);
        break;
      case 'DG3':
        flowMeterConfig = flowMeterConfig.copyWith(dg3: source);
        break;
      case 'DD3':
        flowMeterConfig = flowMeterConfig.copyWith(dd3: source);
        break;
      case 'DG4':
        flowMeterConfig = flowMeterConfig.copyWith(dg4: source);
        break;
      case 'DD4':
        flowMeterConfig = flowMeterConfig.copyWith(dd4: source);
        break;
    }
    await flowMeterConfigService.saveConfig(flowMeterConfig);
    notifyListeners();
  }

  Future<List<BluetoothDevice>> scan() => ble.scanDevices();

  Future<void> connect(BluetoothDevice d) async {
    await ble.connectTo(d);
    _isReconnecting = false; // Connexion réussie, désactiver le flag

    await _bleSub?.cancel();
    _bleSub = ble.jsonStream.listen(_onJson, onError: (e, st) {
      // En cas d'erreur de stream, on notifie pour que l'UI réagisse (potentielle perte)
      notifyListeners();
    });

    // Écouter l'état de reconnexion pour mettre à jour l'UI
    await _connectionStateSub?.cancel();
    _connectionStateSub = ble.connectionStateStream.listen((isConnected) {
      _isReconnecting = ble.isReconnecting;
      if (!isConnected) {
        // Déconnexion ou reconnexion en cours - réinitialisation des données
        _resetData();
      }
      notifyListeners();
    });

    notifyListeners();
  }

  Future<void> disconnect() async {
    await ble.disconnect();

    await _bleSub?.cancel();
    _bleSub = null;

    await _connectionStateSub?.cancel();
    _connectionStateSub = null;

    _isReconnecting = false;

    // Réinitialiser les données et l'état
    _resetData();
    notifyListeners();
  }

  /// 🔄 Réinitialise toutes les données à zéro lors d'une déconnexion
  void _resetData() {
    data = CapteurData.zero();
    _filter.reset();
    lastTick = null;
    lastJson = null;
    _aliveTimer?.cancel();
    _aliveTimer = null;
    _lastFrameIds.clear();
    _hasPacketLoss = false;
  }

  void _onJson(String s) {
    print("📩 Trame reçue BLE : $s");
    lastJson = s;

    try {
      final Map<String, dynamic> j = jsonDecode(s);
      print("✅ JSON décodé avec succès : $j");

      // 🔍 Détection du type de message
      if (j.containsKey('fw_version')) {
        // C'est une réponse GET_INFO
        firmwareInfo = FirmwareInfo.fromJson(j);
        print("✅ Infos firmware reçues : ${firmwareInfo.version}");
        notifyListeners();
        return;
      }

      if (j.containsKey('coeff')) {
        // C'est une réponse GET_COEFF - ne pas la traiter ici
        // (elle est gérée dans calibration_screen directement)
        return;
      }

      if (j.containsKey('event')) {
        // Événement OTA (ota_ready, ota_progress, ota_success...) - ne pas le
        // traiter ici (géré par performOtaTransfer / firmware_update_screen)
        return;
      }

      // Sinon, c'est une trame data normale
      // On applique la moyenne glissante sur les valeurs brutes avant affichage.
      data = _filter.process(CapteurData.fromJson(j));
      lastTick = DateTime.now();

      // 📊 Détection de perte de trames via suivi glissant des IDs
      _checkFrameLoss(data.ID);

      // Passe immédiatement au vert
      notifyListeners();

      // Programme le retour au rouge après tickDuration
      _scheduleAliveRefresh();

    } catch (e) {
      print("⛔ Erreur JSON : $e");
      // Même en cas de JSON invalide, on considère que "quelque chose" est arrivé
      // pour ne pas rester bloqué au rouge si le lien est bien vivant.
      lastTick = DateTime.now();
      notifyListeners();
      _scheduleAliveRefresh();
    }
  }

  void _checkFrameLoss(int newId) {
    //TODO implémenter   if (!isConnected) return;
    // Ignorer les doublons d'ID (même trame reçue plusieurs fois)
    if (_lastFrameIds.isNotEmpty && newId == _lastFrameIds.last) {
      return; // on ne fait rien, trame répétée
    }

    _lastFrameIds.add(newId);
    if (_lastFrameIds.length > _maxFrameHistory) {
      _lastFrameIds.removeAt(0);
    }

    if (_lastFrameIds.length < 2) return;

    final prev = _lastFrameIds[_lastFrameIds.length - 2];
    final curr = _lastFrameIds.last;

    // 🔹 Détection de perte
    final expected = prev + 1;
    if (curr != expected) {
      if (!_hasPacketLoss) {
        print("⚠️ Perte de trame : attendu $expected, reçu $curr");
        _hasPacketLoss = true;
      }
      return;
    }

    // 🔹 Vérification retour à la normale
    if (_hasPacketLoss) {
      const recoveryWindow = 5;
      if (_lastFrameIds.length >= recoveryWindow) {
        final recent = _lastFrameIds.sublist(_lastFrameIds.length - recoveryWindow);
        bool allConsecutive = true;
        for (int i = 1; i < recent.length; i++) {
          if (recent[i] != recent[i - 1] + 1) {
            allConsecutive = false;
            break;
          }
        }
        if (allConsecutive) {
          print("✅ Retour à la normale : $recoveryWindow trames consécutives");
          _hasPacketLoss = false;
        }
      }
    }
  }


  void _scheduleAliveRefresh() {
    // Annule l'ancien timer et reprogramme un nouveau
    _aliveTimer?.cancel();
    _aliveTimer = Timer(
      // + petit delta pour être sûr de dépasser strictement la condition (<=)
      tickDuration + const Duration(milliseconds: 30),
      () {
        // Re-notifie pour recalculer isAlive et repasser au rouge si nécessaire
        notifyListeners();
      },
    );
  }

  // Simulation ponctuelle (bouton)
  void pushSimulatedFrame(int pairs) {
    final s = sim.generateJson(pairs: pairs);
    _onJson(s);
  }

  // Calibration – proxies
  Future<void> requestCoefficients() => ble.requestCoefficients();
  Future<void> sendUpdatedCoefficients(Map<String, dynamic> coeff) =>
      ble.sendUpdatedCoefficients(coeff);

  // Firmware info – proxy
  Future<void> requestFirmwareInfo() => ble.requestFirmwareInfo();

  @override
  void dispose() {
    _aliveTimer?.cancel();
    _bleSub?.cancel();
    _connectionStateSub?.cancel();
    super.dispose();
  }
}
