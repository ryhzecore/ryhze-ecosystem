import 'dart:io';

import '../../providers/public_artwork_repository.dart';
import 'windows_artwork_cache.dart';

import '../../features/store/catalog_repository.dart';
import '../../data/transaction_store.dart';
import '../../providers/public_catalog_cache.dart';
import '../../features/installations/installation_repository.dart';
import 'windows_installations.dart';
import 'windows_content_staging.dart';
import 'windows_content_transfer.dart';
import '../../features/installations/content_transfer_backend.dart';
import '../../features/installations/content_staging_backend.dart';
import '../../providers/ryhze_catalog_repository.dart';
import 'windows_activation.dart';
import 'windows_security.dart';
import 'windows_updates.dart';
import 'windows_taskbar.dart';
import 'windows_notifications.dart';

CatalogRepository? createWindowsCatalog([TransactionStore? store]) =>
    Platform.isWindows
    ? RyhzeCatalogRepository(
        cache: store == null ? null : PublicCatalogCache(store),
      )
    : null;

Future<WindowsActivationClient?> connectWindowsActivation() async =>
    Platform.isWindows ? WindowsActivationClient.connect() : null;
Future<WindowsSecurityClient?> connectWindowsSecurity() async =>
    Platform.isWindows ? WindowsSecurityClient.connect() : null;
Future<WindowsUpdateClient?> connectWindowsUpdates() async =>
    Platform.isWindows ? WindowsUpdateClient.connect() : null;
Future<WindowsTaskbarClient?> connectWindowsTaskbar() async =>
    Platform.isWindows ? WindowsTaskbarClient.connect() : null;
Future<WindowsNotificationsClient?> connectWindowsNotifications() async =>
    Platform.isWindows ? WindowsNotificationsClient.connect() : null;
Future<InstallationRepository?> connectWindowsInstallations() async =>
    Platform.isWindows ? WindowsInstallationRepository.connect() : null;

Future<ContentStagingBackend?> connectWindowsContentStaging() async =>
    Platform.isWindows ? WindowsContentStaging.connect() : null;

Future<ContentTransferBackend?> connectWindowsContentTransfer() async =>
    Platform.isWindows ? WindowsContentTransfer.connect() : null;

Future<PublicArtworkRepository?> connectWindowsArtwork() async =>
    Platform.isWindows
    ? PublicArtworkRepository(cache: await WindowsArtworkCache.connect())
    : null;
