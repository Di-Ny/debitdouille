import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'package:wakelock_plus/wakelock_plus.dart';

import '../models/firmware_info.dart';
import '../models/firmware_release.dart';
import '../providers/data_provider.dart';
import '../services/firmware_update_service.dart';
import '../utils/app_version.dart';
import '../utils/constants.dart';

/// Écran "Mise à jour firmware" : compare la version de l'ESP connecté à la
/// dernière release GitHub, puis pilote le téléchargement et le transfert OTA
/// (protocole docs/ota.md).
class FirmwareUpdateScreen extends StatefulWidget {
  const FirmwareUpdateScreen({super.key});

  @override
  State<FirmwareUpdateScreen> createState() => _FirmwareUpdateScreenState();
}

enum _Phase {
  checking, // lecture infos ESP + requête GitHub
  notConnected, // pas de périphérique BLE
  error, // erreur (message dans _errorMessage), bouton réessayer
  upToDate, // firmware à jour
  appTooOld, // MAJ firmware dispo mais l'app doit d'abord être mise à jour
  updateAvailable, // MAJ dispo, bouton "Mettre à jour"
  downloading, // téléchargement du binaire GitHub
  transferring, // envoi BLE vers l'ESP
  rebooting, // ESP a confirmé, attente reboot + reconnexion
  success, // nouvelle version vérifiée après reboot
}

class _FirmwareUpdateScreenState extends State<FirmwareUpdateScreen> {
  final _updateService = FirmwareUpdateService();

  _Phase _phase = _Phase.checking;
  String _errorMessage = '';
  FirmwareInfo? _espInfo;
  FirmwareRelease? _release;
  double _progress = 0; // 0..1 pour downloading/transferring
  String _verifiedVersion = ''; // version relue après reboot
  bool _binaryCached = false; // binaire déjà sur disque → installable hors connexion

  bool get _busy =>
      _phase == _Phase.downloading ||
      _phase == _Phase.transferring ||
      _phase == _Phase.rebooting;

  @override
  void initState() {
    super.initState();
    _check();
  }

  @override
  void dispose() {
    WakelockPlus.disable();
    super.dispose();
  }

  /// Lit la version du firmware connecté via get_info (réponse attendue sur le
  /// flux JSON), indépendamment du DataProvider pour pouvoir attendre avec timeout.
  Future<FirmwareInfo> _readEspInfo() async {
    final ble = context.read<DataProvider>().ble;
    final completer = Completer<FirmwareInfo>();
    final sub = ble.jsonStream.listen((s) {
      try {
        final j = jsonDecode(s);
        if (j is Map<String, dynamic> && j.containsKey('fw_version')) {
          if (!completer.isCompleted) completer.complete(FirmwareInfo.fromJson(j));
        }
      } catch (_) {}
    });
    try {
      await ble.requestFirmwareInfo();
      return await completer.future.timeout(
        const Duration(seconds: 8),
        onTimeout: () => throw Exception(
            "L'ESP ne répond pas à la demande de version. Vérifiez la connexion."),
      );
    } finally {
      await sub.cancel();
    }
  }

  Future<void> _check({bool forceRefresh = false}) async {
    setState(() {
      _phase = _Phase.checking;
      _errorMessage = '';
    });

    final dataProvider = context.read<DataProvider>();
    if (dataProvider.connectedDevice == null) {
      setState(() => _phase = _Phase.notConnected);
      return;
    }

    try {
      final info = await _readEspInfo();
      final release = await _updateService.fetchLatestRelease(forceRefresh: forceRefresh);
      final binary = release?.binaryFor(info.espModel);
      final cached = binary != null && await _updateService.isBinaryCached(binary);

      if (!mounted) return;
      setState(() {
        _binaryCached = cached;
        _espInfo = info;
        _release = release;
        if (release == null ||
            compareVersions(release.version, info.version) <= 0) {
          _phase = _Phase.upToDate;
        } else if (release.minAppVersion.isNotEmpty &&
            compareVersions(AppVersion.version, release.minAppVersion) < 0) {
          _phase = _Phase.appTooOld;
        } else if (release.binaryFor(info.espModel) == null) {
          _phase = _Phase.error;
          _errorMessage =
              "La release ${release.version} ne contient pas de binaire pour "
              "le modèle « ${info.espModel} ».";
        } else {
          _phase = _Phase.updateAvailable;
        }
      });
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _phase = _Phase.error;
        _errorMessage = e.toString().replaceFirst('Exception: ', '');
      });
    }
  }

  /// Sélecteur de version : liste toutes les releases publiées (récentes en
  /// tête) et permet d'en installer une précise — y compris plus ancienne que
  /// l'actuelle, pour revenir en arrière après une régression.
  Future<void> _chooseVersion() async {
    final List<FirmwareReleaseSummary> releases;
    try {
      releases = await _updateService.fetchReleaseList();
    } catch (e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
          content: Text("Liste des versions indisponible : "
              "${e.toString().replaceFirst('Exception: ', '')}")));
      return;
    }
    if (!mounted || releases.isEmpty) return;

    final installed = _espInfo?.version ?? '';
    final summary = await showModalBottomSheet<FirmwareReleaseSummary>(
      context: context,
      backgroundColor: const Color(0xFF1A1A1A),
      builder: (ctx) => SafeArea(
        child: ListView(
          shrinkWrap: true,
          children: [
            const Padding(
              padding: EdgeInsets.all(16),
              child: Text("Choisir une version à installer",
                  style: TextStyle(
                      color: Colors.white,
                      fontSize: 18,
                      fontWeight: FontWeight.bold)),
            ),
            for (final r in releases)
              ListTile(
                title: Text(r.version,
                    style: const TextStyle(color: Colors.white, fontSize: 16)),
                subtitle: Text(
                  r.publishedAt.isNotEmpty ? r.publishedAt : r.tagName,
                  style: const TextStyle(color: Colors.white54, fontSize: 13),
                ),
                trailing: compareVersions(r.version, installed) == 0
                    ? const Text("installée",
                        style: TextStyle(color: Colors.green, fontSize: 13))
                    : compareVersions(r.version, installed) < 0
                        ? const Icon(Icons.history, color: Colors.orange, size: 20)
                        : const Icon(Icons.upgrade, color: Colors.white54, size: 20),
                onTap: () => Navigator.of(ctx).pop(r),
              ),
          ],
        ),
      ),
    );
    if (summary == null || !mounted) return;

    // Confirmation explicite, avec avertissement en cas de retour arrière
    final isDowngrade = compareVersions(summary.version, installed) < 0;
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        backgroundColor: const Color(0xFF1A1A1A),
        title: Text("Installer la version ${summary.version} ?",
            style: const TextStyle(color: Colors.white)),
        content: Text(
          "Firmware actuel : $installed.\n"
          "${isDowngrade ? "⚠️ Vous installez une version plus ancienne que "
              "l'actuelle. Les réglages sont conservés mais certaines "
              "fonctionnalités récentes peuvent disparaître.\n\n" : ""}"
          "Le transfert Bluetooth prend plusieurs minutes.",
          style: const TextStyle(color: Colors.white70),
        ),
        actions: [
          TextButton(
              onPressed: () => Navigator.of(ctx).pop(false),
              child: const Text("Annuler")),
          TextButton(
              onPressed: () => Navigator.of(ctx).pop(true),
              child: const Text("Installer")),
        ],
      ),
    );
    if (confirmed != true || !mounted) return;

    try {
      final release = await _updateService.fetchRelease(summary);
      if (!mounted) return;
      if (release.binaryFor(_espInfo!.espModel) == null) {
        setState(() {
          _phase = _Phase.error;
          _errorMessage = "La version ${release.version} ne contient pas de "
              "binaire pour le modèle « ${_espInfo!.espModel} ».";
        });
        return;
      }
      if (release.minAppVersion.isNotEmpty &&
          compareVersions(AppVersion.version, release.minAppVersion) < 0) {
        setState(() {
          _phase = _Phase.error;
          _errorMessage = "La version ${release.version} nécessite "
              "l'application ${release.minAppVersion} minimum.";
        });
        return;
      }
      await _startUpdate(release);
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _phase = _Phase.error;
        _errorMessage = e.toString().replaceFirst('Exception: ', '');
      });
    }
  }

  /// Télécharge le binaire dans le cache disque sans flasher : permet de
  /// préparer la mise à jour en Wi-Fi puis de l'installer au champ hors connexion.
  Future<void> _predownload() async {
    final release = _release!;
    final binary = release.binaryFor(_espInfo!.espModel)!;
    setState(() {
      _phase = _Phase.downloading;
      _progress = 0;
    });
    try {
      await _updateService.downloadBinary(
        binary,
        onProgress: (received, total) {
          if (mounted && total > 0) setState(() => _progress = received / total);
        },
      );
      if (!mounted) return;
      setState(() {
        _binaryCached = true;
        _phase = _Phase.updateAvailable;
      });
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _phase = _Phase.error;
        _errorMessage = e.toString().replaceFirst('Exception: ', '');
      });
    }
  }

  /// Lance la mise à jour vers [target] (par défaut la dernière release
  /// détectée) — sert aussi à installer une version plus ancienne.
  Future<void> _startUpdate([FirmwareRelease? target]) async {
    final dataProvider = context.read<DataProvider>();
    final ble = dataProvider.ble;
    final release = target ?? _release!;
    final info = _espInfo!;
    final binary = release.binaryFor(info.espModel)!;

    await WakelockPlus.enable(); // l'écran doit rester allumé pendant le transfert
    try {
      // 1. Téléchargement depuis GitHub + vérification SHA-256 locale
      setState(() {
        _phase = _Phase.downloading;
        _progress = 0;
      });
      final Uint8List firmware = await _updateService.downloadBinary(
        binary,
        onProgress: (received, total) {
          if (mounted && total > 0) setState(() => _progress = received / total);
        },
      );

      // 2. Transfert BLE (l'ESP vérifie modèle + SHA puis reboote tout seul)
      setState(() {
        _phase = _Phase.transferring;
        _progress = 0;
      });
      await ble.performOtaTransfer(
        firmware: firmware,
        sha256Hex: binary.sha256,
        espModel: info.espModel,
        fwVersion: release.version,
        onProgress: (sent, total) {
          if (mounted) setState(() => _progress = sent / total);
        },
      );

      // 3. Reboot de l'ESP : la reconnexion automatique du BleService prend le
      // relais ; on attend le retour de la connexion puis on revérifie la version.
      setState(() => _phase = _Phase.rebooting);
      await ble.connectionStateStream
          .where((connected) => connected)
          .first
          .timeout(
            const Duration(seconds: 90),
            onTimeout: () => throw Exception(
                "L'ESP ne s'est pas reconnecté après la mise à jour. "
                "Rapprochez-vous de l'appareil et reconnectez-vous manuellement."),
          );
      // Petit délai : laisser l'ESP finir son démarrage avant de l'interroger
      await Future.delayed(const Duration(seconds: 2));
      final newInfo = await _readEspInfo();

      if (!mounted) return;
      setState(() {
        _verifiedVersion = newInfo.version;
        _espInfo = newInfo;
        // Égalité stricte : la cible peut aussi être une version plus ancienne
        if (compareVersions(newInfo.version, release.version) == 0) {
          _phase = _Phase.success;
        } else {
          _phase = _Phase.error;
          _errorMessage =
              "L'ESP s'est reconnecté mais annonce la version "
              "${newInfo.version} (attendu ${release.version}).";
        }
      });
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _phase = _Phase.error;
        _errorMessage = e.toString().replaceFirst('Exception: ', '');
      });
    } finally {
      await WakelockPlus.disable();
    }
  }

  @override
  Widget build(BuildContext context) {
    return PopScope(
      // Pas de retour pendant le transfert : une interruption = MAJ à recommencer
      canPop: !_busy,
      child: Scaffold(
        backgroundColor: AppColors.background,
        appBar: AppBar(
          backgroundColor: Colors.black,
          title: const Text("Mise à jour firmware",
              style: TextStyle(color: Colors.white)),
          iconTheme: const IconThemeData(color: Colors.white),
        ),
        body: Padding(
          padding: const EdgeInsets.all(16),
          child: ListView(
            children: [
              _buildVersionHeader(),
              const SizedBox(height: 24),
              ..._buildPhaseContent(),
            ],
          ),
        ),
      ),
    );
  }

  Widget _buildVersionHeader() {
    const labelStyle = TextStyle(color: Colors.white54, fontSize: 14);
    const valueStyle = TextStyle(
        color: Colors.white, fontSize: 16, fontWeight: FontWeight.w500);
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        if (_espInfo != null) ...[
          const Text("Firmware installé", style: labelStyle),
          Text("${_espInfo!.version} — ${_espInfo!.espModel}", style: valueStyle),
          const SizedBox(height: 8),
        ],
        if (_release != null) ...[
          const Text("Dernière version publiée", style: labelStyle),
          Text(
            "${_release!.version}"
            "${_release!.releaseDate.isNotEmpty ? ' (${_release!.releaseDate})' : ''}",
            style: valueStyle,
          ),
        ],
      ],
    );
  }

  List<Widget> _buildPhaseContent() {
    const msgStyle = TextStyle(color: Colors.white, fontSize: 16);
    switch (_phase) {
      case _Phase.checking:
        return const [
          Center(child: CircularProgressIndicator(color: Colors.white)),
          SizedBox(height: 16),
          Center(
              child: Text("Vérification des versions...", style: msgStyle)),
        ];

      case _Phase.notConnected:
        return [
          const Text(
            "Connectez-vous d'abord au dispositif en Bluetooth pour vérifier "
            "sa version et le mettre à jour.",
            style: msgStyle,
          ),
          const SizedBox(height: 16),
          _actionButton("Réessayer", Icons.refresh, () => _check()),
        ];

      case _Phase.error:
        return [
          Row(
            children: [
              const Icon(Icons.error_outline, color: Colors.orange),
              const SizedBox(width: 8),
              Expanded(child: Text(_errorMessage, style: msgStyle)),
            ],
          ),
          const SizedBox(height: 16),
          _actionButton("Réessayer", Icons.refresh,
              () => _check(forceRefresh: true)),
        ];

      case _Phase.upToDate:
        return [
          Row(
            children: const [
              Icon(Icons.check_circle_outline, color: Colors.green),
              SizedBox(width: 8),
              Expanded(
                  child: Text("Le firmware est à jour.", style: msgStyle)),
            ],
          ),
          const SizedBox(height: 16),
          _actionButton("Revérifier", Icons.refresh,
              () => _check(forceRefresh: true)),
          const SizedBox(height: 12),
          _secondaryButton("Installer une autre version…", Icons.history,
              _chooseVersion),
        ];

      case _Phase.appTooOld:
        return [
          Text(
            "Une mise à jour du firmware (${_release!.version}) est disponible, "
            "mais elle nécessite l'application en version "
            "${_release!.minAppVersion} minimum (installée : ${AppVersion.version}).\n\n"
            "Mettez d'abord l'application à jour depuis le Play Store.",
            style: msgStyle,
          ),
        ];

      case _Phase.updateAvailable:
        return [
          Text(
            "Mise à jour ${_release!.version} disponible.",
            style: const TextStyle(
                color: Colors.white, fontSize: 18, fontWeight: FontWeight.bold),
          ),
          if (_release!.changelog.isNotEmpty) ...[
            const SizedBox(height: 12),
            const Text("Nouveautés :",
                style: TextStyle(color: Colors.white54, fontSize: 14)),
            const SizedBox(height: 4),
            Text(_release!.changelog,
                style: const TextStyle(color: Colors.white70, fontSize: 14)),
          ],
          const SizedBox(height: 12),
          const Text(
            "Le transfert se fait en Bluetooth et peut prendre plusieurs "
            "minutes. Restez à proximité de l'appareil et laissez "
            "l'application ouverte.",
            style: TextStyle(color: Colors.white54, fontSize: 14),
          ),
          const SizedBox(height: 16),
          _actionButton("Mettre à jour", Icons.system_update_alt, _startUpdate),
          const SizedBox(height: 12),
          // Plusieurs mises à jour de retard ? Revérifier sans quitter l'écran.
          _secondaryButton("Revérifier", Icons.refresh,
              () => _check(forceRefresh: true)),
          const SizedBox(height: 12),
          _secondaryButton("Installer une autre version…", Icons.history,
              _chooseVersion),
          const SizedBox(height: 12),
          if (_binaryCached)
            Row(
              children: const [
                Icon(Icons.offline_pin, color: Colors.green, size: 20),
                SizedBox(width: 8),
                Expanded(
                  child: Text(
                    "Firmware déjà téléchargé — installation possible même "
                    "sans connexion Internet.",
                    style: TextStyle(color: Colors.white70, fontSize: 14),
                  ),
                ),
              ],
            )
          else
            _secondaryButton(
                "Pré-télécharger (installation hors connexion plus tard)",
                Icons.download,
                _predownload),
        ];

      case _Phase.downloading:
        return _progressContent("Téléchargement du firmware...");

      case _Phase.transferring:
        return _progressContent(
            "Envoi vers l'ESP32 en Bluetooth...\nNe fermez pas l'application.");

      case _Phase.rebooting:
        return const [
          Center(child: CircularProgressIndicator(color: Colors.white)),
          SizedBox(height: 16),
          Center(
            child: Text(
              "Firmware transféré et vérifié.\nRedémarrage de l'ESP32 et "
              "reconnexion en cours...",
              style: msgStyle,
              textAlign: TextAlign.center,
            ),
          ),
        ];

      case _Phase.success:
        return [
          Row(
            children: [
              const Icon(Icons.check_circle, color: Colors.green, size: 32),
              const SizedBox(width: 12),
              Expanded(
                child: Text(
                  "Mise à jour réussie !\nL'ESP32 fonctionne maintenant en "
                  "version $_verifiedVersion.",
                  style: msgStyle,
                ),
              ),
            ],
          ),
          const SizedBox(height: 16),
          _actionButton("Fermer", Icons.done, () => Navigator.of(context).pop()),
        ];
    }
  }

  List<Widget> _progressContent(String label) {
    return [
      Text(label,
          style: const TextStyle(color: Colors.white, fontSize: 16),
          textAlign: TextAlign.center),
      const SizedBox(height: 16),
      LinearProgressIndicator(
        value: _progress > 0 ? _progress : null,
        color: Colors.white,
        backgroundColor: Colors.white24,
        minHeight: 8,
      ),
      const SizedBox(height: 8),
      Center(
        child: Text(
          "${(_progress * 100).toStringAsFixed(0)} %",
          style: const TextStyle(color: Colors.white70, fontSize: 14),
        ),
      ),
    ];
  }

  Widget _actionButton(String label, IconData icon, VoidCallback onPressed) {
    return Center(
      child: ElevatedButton.icon(
        onPressed: onPressed,
        icon: Icon(icon),
        label: Text(label),
        style: ElevatedButton.styleFrom(
          backgroundColor: Colors.white,
          foregroundColor: Colors.black,
          padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 12),
        ),
      ),
    );
  }

  Widget _secondaryButton(String label, IconData icon, VoidCallback onPressed) {
    return Center(
      child: OutlinedButton.icon(
        onPressed: onPressed,
        icon: Icon(icon, color: Colors.white),
        label: Text(label,
            style: const TextStyle(color: Colors.white),
            textAlign: TextAlign.center),
        style: OutlinedButton.styleFrom(
          side: const BorderSide(color: Colors.white54),
          padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 10),
        ),
      ),
    );
  }
}
