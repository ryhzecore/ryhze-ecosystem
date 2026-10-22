import '../../features/store/catalog_repository.dart';
import '../../providers/public_artwork_repository.dart';

import '../../data/transaction_store.dart';
import '../../features/installations/installation_repository.dart';
import 'windows_activation.dart';
import 'windows_security.dart';
import 'windows_updates.dart';
import 'windows_taskbar.dart';
import 'windows_notifications.dart';
import '../../features/installations/content_staging_backend.dart';
import '../../features/installations/content_transfer_backend.dart';

Future<ContentStagingBackend?> connectWindowsContentStaging() async => null;
Future<ContentTransferBackend?> connectWindowsContentTransfer() async => null;

Future<WindowsUpdateClient?> connectWindowsUpdates() async => null;
Future<WindowsTaskbarClient?> connectWindowsTaskbar() async => null;
Future<WindowsNotificationsClient?> connectWindowsNotifications() async => null;
Future<InstallationRepository?> connectWindowsInstallations() async => null;

CatalogRepository? createWindowsCatalog([TransactionStore? store]) => null;

Future<WindowsActivationClient?> connectWindowsActivation() async => null;
Future<WindowsSecurityClient?> connectWindowsSecurity() async => null;
Future<PublicArtworkRepository?> connectWindowsArtwork() async => null;
