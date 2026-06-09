import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../providers/settings_provider.dart';
import '../providers/data_provider.dart';
import '../utils/constants.dart';
import '../utils/app_version.dart';
import '../widgets/value_block.dart';
import 'firmware_update_screen.dart';

class SettingsScreen extends StatelessWidget {
  const SettingsScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final s = context.watch<SettingsProvider>();
    final dataProvider = context.watch<DataProvider>();

    const titleStyle = TextStyle(color: Colors.white70, fontSize: 20, fontWeight: FontWeight.bold);
    const sectionLabelStyle = TextStyle(color: Colors.white, fontSize: 18, fontWeight: FontWeight.w500);
    const smallLabelStyle = TextStyle(color: Colors.white54, fontSize: 16);

    return Container(
      color: AppColors.background,
      padding: const EdgeInsets.all(16),
      child: ListView(
        children: [
          const Text("Paramètres d'affichage", style: titleStyle),
          const SizedBox(height: 16),

          // ---- Paires de débits ----
          Column(
            crossAxisAlignment: CrossAxisAlignment.center,
            children: [
              const Text("Paires de débits :", style: sectionLabelStyle),
              const SizedBox(height: 12),
              Row(
                mainAxisAlignment: MainAxisAlignment.center,
                children: List.generate(4, (i) {
                  final value = i + 1;
                  final isSelected = s.pairs == value;
                  return Padding(
                    padding: const EdgeInsets.only(right: 8.0),
                    child: OutlinedButton(
                      style: OutlinedButton.styleFrom(
                        backgroundColor: isSelected ? Colors.white : Colors.transparent,
                        side: const BorderSide(color: Colors.white),
                        padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
                      ),
                      onPressed: () => s.setPairs(value),
                      child: Text(
                        value.toString(),
                        style: TextStyle(
                          fontSize: 18,
                          color: isSelected ? Colors.black : Colors.white,
                          fontWeight: isSelected ? FontWeight.bold : FontWeight.normal,
                        ),
                      ),
                    ),
                  );
                }),
              ),
            ],
          ),

          const SizedBox(height: 24),
          const Divider(color: Colors.white24, height: 1),
          const SizedBox(height: 24),

          // ---- Précision d'affichage (nombre de décimales) ----
          Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              const Text("Précision d'affichage", style: sectionLabelStyle),
              const SizedBox(height: 4),
              const Text(
                "Nombre de décimales affichées",
                style: TextStyle(color: Colors.white54, fontSize: 14),
              ),
              const SizedBox(height: 16),
              _buildDecimalsRow("Pression", s.decimalsPressure, s.setDecimalsPressure),
              const SizedBox(height: 12),
              _buildDecimalsRow("Débits", s.decimalsFlow, s.setDecimalsFlow),
              const SizedBox(height: 12),
              _buildDecimalsRow("Vitesse", s.decimalsSpeed, s.setDecimalsSpeed),
            ],
          ),

          const SizedBox(height: 24),
          const Divider(color: Colors.white24, height: 1),
          const SizedBox(height: 24),

          // ---- Taille du texte ----
          Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              const Text("Taille du texte", style: sectionLabelStyle),
              const SizedBox(height: 12),
              Row(
                children: [
                  const Text("Petit", style: smallLabelStyle),
                  Expanded(
                    child: Slider(
                      value: s.fontScale,
                      min: 0.6,
                      max: 2.0,
                      divisions: 6,
                      label: "${(s.fontScale * 100).round()}%",
                      onChanged: (v) => s.setFontScale(v),
                      activeColor: Colors.white,
                      inactiveColor: Colors.white24,
                    ),
                  ),
                  const Text("Grand", style: smallLabelStyle),
                ],
              ),
              const SizedBox(height: 12),

              // ✅ Aperçu fiable : réutilise le même composant que la Home
              Center(
                child: ValueBlock(
                  label: "Aperçu",
                  value: "12.34",
                  unit: "bar",
                  fontScale: 1.0, // facteur local: 1.0
                ),
              ),
            ],
          ),

          const SizedBox(height: 24),
          const Divider(color: Colors.white24, height: 1),
          const SizedBox(height: 24),

          // ---- Debug / Simulation ----
          SwitchListTile(
            value: s.showDebug,
            onChanged: (v) => s.setShowDebug(v),
            title: const Text("Afficher trame JSON (debug)", style: sectionLabelStyle),
            activeColor: Colors.white,
          ),
          SwitchListTile(
            value: s.showSimButton,
            onChanged: (v) => s.setShowSimButton(v),
            title: const Text("Afficher bouton Simulation", style: sectionLabelStyle),
            activeColor: Colors.white,
          ),

          const SizedBox(height: 24),
          const Divider(color: Colors.white24, height: 1),
          const SizedBox(height: 24),

          // ---- Informations Versions ----
          Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              const Text("Informations Versions", style: sectionLabelStyle),
              const SizedBox(height: 16),

              // Affichage des informations firmware hardware
              if (dataProvider.firmwareInfo.version != "Non disponible") ...[
                _buildInfoRow(
                  "Version hardware",
                  "${dataProvider.firmwareInfo.version} [${dataProvider.firmwareInfo.buildDate} ${dataProvider.firmwareInfo.buildTime}]",
                  smallLabelStyle,
                ),
                const SizedBox(height: 8),
                _buildInfoRow("Modèle hardware", dataProvider.firmwareInfo.espModel, smallLabelStyle),
                const SizedBox(height: 16),
              ] else ...[
                const Padding(
                  padding: EdgeInsets.symmetric(vertical: 8.0),
                  child: Text(
                    "Hardware : Aucune information disponible",
                    style: TextStyle(color: Colors.white38, fontSize: 14, fontStyle: FontStyle.italic),
                  ),
                ),
                const SizedBox(height: 16),
              ],

              // Informations version Flutter (lue depuis le pubspec + date de build injectée)
              _buildInfoRow("Version app Flutter", AppVersion.displayVersion, smallLabelStyle),
              const SizedBox(height: 16),

              // Bouton pour requêter les informations hardware
              Center(
                child: ElevatedButton.icon(
                  onPressed: dataProvider.connectedDevice != null
                    ? () => dataProvider.requestFirmwareInfo()
                    : null,
                  icon: const Icon(Icons.info_outline),
                  label: const Text("Obtenir infos hardware"),
                  style: ElevatedButton.styleFrom(
                    backgroundColor: Colors.white,
                    foregroundColor: Colors.black,
                    disabledBackgroundColor: Colors.white24,
                    disabledForegroundColor: Colors.white38,
                    padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 12),
                  ),
                ),
              ),

              const SizedBox(height: 12),

              // Mise à jour OTA du firmware ESP32 (voir docs/ota.md)
              Center(
                child: ElevatedButton.icon(
                  onPressed: dataProvider.connectedDevice != null
                    ? () => Navigator.of(context).push(
                        MaterialPageRoute(
                          builder: (_) => const FirmwareUpdateScreen(),
                        ),
                      )
                    : null,
                  icon: const Icon(Icons.system_update_alt),
                  label: const Text("Mise à jour firmware"),
                  style: ElevatedButton.styleFrom(
                    backgroundColor: Colors.white,
                    foregroundColor: Colors.black,
                    disabledBackgroundColor: Colors.white24,
                    disabledForegroundColor: Colors.white38,
                    padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 12),
                  ),
                ),
              ),

              if (dataProvider.connectedDevice == null)
                const Padding(
                  padding: EdgeInsets.only(top: 8.0),
                  child: Center(
                    child: Text(
                      "Connectez-vous d'abord au dispositif BLE",
                      style: TextStyle(color: Colors.orange, fontSize: 12),
                    ),
                  ),
                ),
            ],
          ),
        ],
      ),
    );
  }

  Widget _buildDecimalsRow(String label, int current, void Function(int) onSelect) {
    return Row(
      children: [
        SizedBox(
          width: 100,
          child: Text(
            label,
            style: const TextStyle(color: Colors.white, fontSize: 16),
          ),
        ),
        Row(
          children: List.generate(4, (value) {
            final isSelected = current == value;
            return Padding(
              padding: const EdgeInsets.only(right: 8.0),
              child: OutlinedButton(
                style: OutlinedButton.styleFrom(
                  backgroundColor: isSelected ? Colors.white : Colors.transparent,
                  side: const BorderSide(color: Colors.white),
                  padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
                  minimumSize: const Size(0, 0),
                ),
                onPressed: () => onSelect(value),
                child: Text(
                  value.toString(),
                  style: TextStyle(
                    fontSize: 16,
                    color: isSelected ? Colors.black : Colors.white,
                    fontWeight: isSelected ? FontWeight.bold : FontWeight.normal,
                  ),
                ),
              ),
            );
          }),
        ),
      ],
    );
  }

  Widget _buildInfoRow(String label, String value, TextStyle labelStyle) {
    return Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        SizedBox(
          width: 160,
          child: Text(
            "$label :",
            style: labelStyle,
          ),
        ),
        Expanded(
          child: Text(
            value,
            style: const TextStyle(color: Colors.white, fontSize: 16, fontWeight: FontWeight.w500),
          ),
        ),
      ],
    );
  }
}
